// Command controller-v4m is the V4m (plaintext, local-only) home-automation
// controller. It is V4's rule engine (level + trigger, priority, hysteresis,
// debounce) backed by a SQLite database: device registry (all HIOTO categories),
// rules loaded from rule_devices, and telemetry logs. No mTLS — plaintext AMQP.
package main

import (
	"crypto/rand"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"
)

type config struct {
	host       string
	port       int
	vhost      string
	user       string
	password   string
	exchange   string
	periodMS   int
	dbPath     string
	rulesJSON  string
	brokerHost string // public broker address encoded in QR
	mqttPort   int    // plaintext MQTT port encoded in QR
	httpPort   int    // device-management HTTP API port
}

func envOr(key, def string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return def
}
func envIntOr(key string, def int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return def
}
func parseConfig() config {
	var c config
	flag.StringVar(&c.host, "host", envOr("AMQP_HOST", "127.0.0.1"), "AMQP host")
	flag.IntVar(&c.port, "port", envIntOr("AMQP_PORT", 5672), "AMQP port (plaintext)")
	flag.StringVar(&c.vhost, "vhost", envOr("AMQP_VHOST", "/"), "vhost")
	flag.StringVar(&c.user, "user", envOr("AMQP_USER", "admin"), "username")
	flag.StringVar(&c.password, "password", envOr("AMQP_PASS", "123456Aa!"), "password")
	flag.StringVar(&c.exchange, "exchange", envOr("EXCHANGE", "home.automation"), "topic exchange")
	flag.IntVar(&c.periodMS, "period-ms", envIntOr("PERIOD_MS", 1000), "control loop cadence")
	flag.StringVar(&c.dbPath, "db", envOr("DB_PATH", "/var/lib/homeautomation/v4m.db"), "SQLite database")
	flag.StringVar(&c.rulesJSON, "rules-json", envOr("RULES_JSON", ""), "rule_devices JSON export to import on startup")
	flag.StringVar(&c.brokerHost, "broker-host", envOr("BROKER_HOST", "192.168.137.44"), "public broker address (for QR)")
	flag.IntVar(&c.mqttPort, "mqtt-port", envIntOr("MQTT_PORT", 1883), "plaintext MQTT port (for QR)")
	flag.IntVar(&c.httpPort, "http-port", envIntOr("HTTP_PORT", 8081), "device-management HTTP API port")
	flag.Parse()
	return c
}

// ---------------------------------------------------------------------------
// Wire types
// ---------------------------------------------------------------------------
type envelope struct {
	MsgID        string          `json:"msg_id"`
	TS           time.Time       `json:"ts"`
	Source       string          `json:"source"`
	MessageClass string          `json:"message_class"`
	Payload      json.RawMessage `json:"payload"`
}

type statePayload struct {
	SerialNumber string    `json:"serial_number"`
	DigitalValue []int     `json:"digital_value"`
	AnalogValue  []float64 `json:"analog_value"`
}

type overridePayload struct {
	SerialNumber string `json:"serial_number"`
	Override     bool   `json:"override"`
}

type condition struct {
	Type          string  `json:"type,omitempty"`
	Sensor        string  `json:"sensor,omitempty"`
	Op            string  `json:"op,omitempty"`
	Threshold     float64 `json:"threshold,omitempty"`
	Hysteresis    float64 `json:"hysteresis,omitempty"`
	MinDurationMs int     `json:"min_duration_ms,omitempty"`
	At            string  `json:"at,omitempty"`
	From          string  `json:"from,omitempty"`
	To            string  `json:"to,omitempty"`
	ForMinutes    int     `json:"for_minutes,omitempty"`
}

type action struct {
	Actuator string  `json:"actuator"`
	Value    float64 `json:"value"`
}

type rule struct {
	Name     string    `json:"name"`
	Priority int       `json:"priority"`
	Mode     string    `json:"mode,omitempty"`
	When     condition `json:"when"`
	Then     []action  `json:"then"`
	Else     []action  `json:"else"`
}

type ruleMsg struct {
	Action string `json:"action"`
	Name   string `json:"name"`
	Rule   rule   `json:"rule"`
}

func newUUID() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

func minutesOfDay(t time.Time) int { return t.Hour()*60 + t.Minute() }
func parseHHMM(s string) (int, error) {
	p := strings.Split(s, ":")
	if len(p) != 2 {
		return 0, fmt.Errorf("bad time")
	}
	h, e1 := strconv.Atoi(p[0])
	m, e2 := strconv.Atoi(p[1])
	if e1 != nil || e2 != nil || h < 0 || h > 23 || m < 0 || m > 59 {
		return 0, fmt.Errorf("bad time")
	}
	return h*60 + m, nil
}
func atMinute(now time.Time, at string) bool {
	a, err := parseHHMM(at)
	return err == nil && minutesOfDay(now) == a
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
type ruleRun struct {
	out      bool
	raw      bool
	rawSince time.Time
	firedAt  time.Time // when a duration timer's Then fired
}

type state struct {
	mu        sync.RWMutex
	sensors   map[string]float64
	overrides map[string]bool
	actTypes  map[string]int
	rules     map[string]rule
	runs      map[string]*ruleRun
}

func newState() *state {
	return &state{
		sensors:   map[string]float64{},
		overrides: map[string]bool{},
		actTypes:  map[string]int{},
		rules:     map[string]rule{},
		runs:      map[string]*ruleRun{},
	}
}

// sensorKick wakes the control loop immediately when a sensor publishes a new
// value, so event rules (e.g. switch→lamp) fire without waiting for the next
// periodic tick. Buffered to 1 so rapid bursts coalesce into one wake-up.
var sensorKick = make(chan struct{}, 1)

func kickRules() {
	select {
	case sensorKick <- struct{}{}:
	default:
	}
}
func (s *state) putSensor(k string, v float64)       { s.mu.Lock(); s.sensors[k] = v; s.mu.Unlock() }
func (s *state) putOverride(k string, v bool)        { s.mu.Lock(); s.overrides[k] = v; s.mu.Unlock() }
func (s *state) putActType(k string, v int)          { s.mu.Lock(); s.actTypes[k] = v; s.mu.Unlock() }
func (s *state) putRule(r rule) {
	s.mu.Lock()
	s.rules[r.Name] = r
	if _, ok := s.runs[r.Name]; !ok {
		s.runs[r.Name] = &ruleRun{}
	}
	s.mu.Unlock()
}
func (s *state) delRule(n string) { s.mu.Lock(); delete(s.rules, n); delete(s.runs, n); s.mu.Unlock() }
func (s *state) sensor(k string) (float64, bool) {
	s.mu.RLock()
	v, ok := s.sensors[k]
	s.mu.RUnlock()
	return v, ok
}
func (s *state) override(k string) bool { s.mu.RLock(); v := s.overrides[k]; s.mu.RUnlock(); return v }
func (s *state) actType(k string) int   { s.mu.RLock(); v := s.actTypes[k]; s.mu.RUnlock(); return v }

func compare(v, threshold float64, op string) bool {
	switch op {
	case "==":
		return v == threshold
	case "!=":
		return v != threshold
	case ">":
		return v > threshold
	case "<":
		return v < threshold
	case ">=":
		return v >= threshold
	case "<=":
		return v <= threshold
	}
	return false
}

func rawLevel(now time.Time, w condition, get func(string) (float64, bool)) (bool, bool) {
	if w.Type == "now" {
		return true, true // always true: trigger mode fires once on load
	}
	if w.Type == "time" {
		if w.At != "" {
			return atMinute(now, w.At), true
		}
		if w.From != "" || w.To != "" {
			return minutesOfDay(now) >= mustMin(w.From) && minutesOfDay(now) < mustMin(w.To), true
		}
		return false, true
	}
	v, ok := get(w.Sensor)
	if !ok {
		return false, false
	}
	return compare(v, w.Threshold, w.Op), true
}
func mustMin(s string) int {
	m, _ := parseHHMM(s)
	return m
}

func evalLevel(c condition, v float64, run *ruleRun) bool {
	now := time.Now()
	t := c.Threshold
	if run.out {
		switch c.Op {
		case ">", ">=":
			t = c.Threshold - c.Hysteresis
		case "<", "<=":
			t = c.Threshold + c.Hysteresis
		}
	}
	raw := compare(v, t, c.Op)
	if raw != run.raw {
		run.raw = raw
		run.rawSince = now
	}
	if c.MinDurationMs > 0 && now.Sub(run.rawSince) < time.Duration(c.MinDurationMs)*time.Millisecond {
		return run.out
	}
	run.out = raw
	return run.out
}

// ---------------------------------------------------------------------------
// Seed data: 26 V4 devices mapped to HIOTO categories + light rules.
// ---------------------------------------------------------------------------
func seedDefaultDevices(d *DB) {
	devs := []struct{ guid, mac, typ, name string }{
		{"SNS-SW-001", "00:00:00:00:00:01", "SENSOR", "Light Switch 1"},
		{"SNS-SW-002", "00:00:00:00:00:02", "SENSOR", "Light Switch 2"},
		{"SNS-SW-003", "00:00:00:00:00:03", "SENSOR", "Light Switch 3"},
		{"SNS-SW-004", "00:00:00:00:00:04", "SENSOR", "Light Switch 4"},
		{"SNS-SW-005", "00:00:00:00:00:05", "SENSOR", "Light Switch 5"},
		{"SNS-SW-006", "00:00:00:00:00:06", "SENSOR", "Light Switch 6"},
		{"SNS-SW-007", "00:00:00:00:00:07", "SENSOR", "Master Switch"},
		{"ACT-LMP-001", "00:00:00:00:00:11", "AKTUATOR", "Lamp 1"},
		{"ACT-LMP-002", "00:00:00:00:00:12", "AKTUATOR", "Lamp 2"},
		{"ACT-LMP-003", "00:00:00:00:00:13", "AKTUATOR", "Lamp 3"},
		{"ACT-LMP-004", "00:00:00:00:00:14", "AKTUATOR", "Lamp 4"},
		{"ACT-LMP-005", "00:00:00:00:00:15", "AKTUATOR", "Lamp 5"},
		{"ACT-LMP-006", "00:00:00:00:00:16", "AKTUATOR", "Lamp 6"},
		{"ACT-PMP-001", "00:00:00:00:00:17", "AKTUATOR", "Water Pump"},
		{"SNS-PMP-001", "00:00:00:00:00:18", "SENSOR", "Pump Feedback"},
		{"SNS-TMP-001", "00:00:00:00:00:21", "SENSOR_SUHU", "Temperature 1"},
		{"SNS-TMP-002", "00:00:00:00:00:22", "SENSOR_SUHU", "Temperature 2"},
		{"SNS-TMP-003", "00:00:00:00:00:23", "SENSOR_SUHU", "Temperature 3"},
		{"SNS-TMP-004", "00:00:00:00:00:24", "SENSOR_SUHU", "Temperature 4"},
		{"SNS-AIR-001", "00:00:00:00:00:25", "SENSOR_GAS_DETECTOR", "Air Quality"},
		{"SNS-FLW-001", "00:00:00:00:00:26", "SENSOR_WATER_TANK", "Water Flow"},
		{"ACT-AC-001", "00:00:00:00:00:31", "AKTUATOR", "AC 1"},
		{"ACT-AC-002", "00:00:00:00:00:32", "AKTUATOR", "AC 2"},
		{"ACT-AC-003", "00:00:00:00:00:33", "AKTUATOR", "AC 3"},
		{"ACT-AC-004", "00:00:00:00:00:34", "AKTUATOR", "AC 4"},
		{"ACT-APR-001", "00:00:00:00:00:35", "AKTUATOR", "Air Purifier"},
	}
	for _, x := range devs {
		_ = d.upsertDevice(Device{GUID: x.guid, MAC: x.mac, Type: x.typ, Name: x.name})
	}
}

func seedLightRules(d *DB) {
	sw := []string{"SNS-SW-001", "SNS-SW-002", "SNS-SW-003", "SNS-SW-004", "SNS-SW-005", "SNS-SW-006"}
	lp := []string{"ACT-LMP-001", "ACT-LMP-002", "ACT-LMP-003", "ACT-LMP-004", "ACT-LMP-005", "ACT-LMP-006"}
	for i := range sw {
		_ = d.insertRuleDevice(RuleDevice{InputGUID: sw[i], InputValue: 1, OutputGUID: lp[i], OutputValue: 1})
		_ = d.insertRuleDevice(RuleDevice{InputGUID: sw[i], InputValue: 0, OutputGUID: lp[i], OutputValue: 0})
	}
}

// advancedRules are the v4 threshold/hysteresis rules that rule_devices (simple
// equality) cannot express but the controller still supports.
func advancedRules() []rule {
	mk := func(a string, v float64) action { return action{Actuator: a, Value: v} }
	return []rule{
		{Name: "water-pump", When: condition{Sensor: "SNS-FLW-001", Op: "<", Threshold: 2, Hysteresis: 1, MinDurationMs: 5000},
			Then: []action{mk("ACT-PMP-001", 1)}, Else: []action{mk("ACT-PMP-001", 0)}},
		{Name: "ac-1", When: condition{Sensor: "SNS-TMP-001", Op: ">", Threshold: 26, Hysteresis: 1.5},
			Then: []action{mk("ACT-AC-001", 0.8)}, Else: []action{mk("ACT-AC-001", 0.1)}},
		{Name: "ac-2", When: condition{Sensor: "SNS-TMP-002", Op: ">", Threshold: 26, Hysteresis: 1.5},
			Then: []action{mk("ACT-AC-002", 0.8)}, Else: []action{mk("ACT-AC-002", 0.1)}},
		{Name: "ac-3", When: condition{Sensor: "SNS-TMP-003", Op: ">", Threshold: 26, Hysteresis: 1.5},
			Then: []action{mk("ACT-AC-003", 0.8)}, Else: []action{mk("ACT-AC-003", 0.1)}},
		{Name: "ac-4", When: condition{Sensor: "SNS-TMP-004", Op: ">", Threshold: 26, Hysteresis: 1.5},
			Then: []action{mk("ACT-AC-004", 0.8)}, Else: []action{mk("ACT-AC-004", 0.1)}},
		{Name: "air-purifier", When: condition{Sensor: "SNS-AIR-001", Op: ">", Threshold: 100, Hysteresis: 20},
			Then: []action{mk("ACT-APR-001", 1)}, Else: []action{mk("ACT-APR-001", 0.3)}},
	}
}

// rulesFromRuleDevices converts rule_devices rows into level rules. A single
// switch state can drive many lamps, so rows sharing (input_guid, input_value)
// are merged into ONE rule with multiple Then actions.
func rulesFromRuleDevices(rows []RuleDevice) []rule {
	type key struct {
		guid string
		val  float64
	}
	order := []key{}
	groups := map[key][]action{}
	for _, r := range rows {
		k := key{r.InputGUID, r.InputValue}
		if _, ok := groups[k]; !ok {
			order = append(order, k)
		}
		// HIOTO rule_devices already stores the actuator command directly
		// (lamps are active-low: 0=ON, 1=OFF), so forward it verbatim.
		// Direct app control is unaffected (it bypasses the rule engine).
		groups[k] = append(groups[k], action{Actuator: r.OutputGUID, Value: r.OutputValue})
	}
	out := make([]rule, 0, len(order))
	for _, k := range order {
		out = append(out, rule{
			Name: fmt.Sprintf("rd-%s-%s", k.guid, floatStr(k.val)),
			Mode: "trigger", // fire only on switch state change, so a manual
			// (tablet/app) command is not re-asserted every control tick.
			When: condition{Sensor: k.guid, Op: "==", Threshold: k.val},
			Then: groups[k],
		})
	}
	return out
}

func importRulesJSON(d *DB, path string) error {
	b, err := os.ReadFile(path)
	if err != nil {
		return err
	}
	var rows []RuleDevice
	if err := json.Unmarshal(b, &rows); err != nil {
		return err
	}
	return d.importRuleDevices(rows)
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
func main() {
	cfg := parseConfig()
	// Cap rule evaluation at 3 times/second (min 333 ms per tick).
	if cfg.periodMS < 333 {
		cfg.periodMS = 333
	}

	db, err := openDB(cfg.dbPath)
	if err != nil {
		log.Fatalf("db: %v", err)
	}
	defer db.Close()

	// Bounded telemetry: keep ≤10MB in memory, spill to ≤10MB files (≤5).
	initTelemetry(filepath.Join(filepath.Dir(cfg.dbPath), "telemetry"))
	_ = db.pruneLogs(20000) // guard the legacy SQLite logs table against old growth

	// Flush the in-memory telemetry buffer on shutdown so recent readings
	// aren't lost when the controller is stopped or restarted.
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	go func() { <-sig; tel.flush(); os.Exit(0) }()

	// Import rule_devices from a JSON export if provided; otherwise seed defaults.
	if cfg.rulesJSON != "" {
		if err := importRulesJSON(db, cfg.rulesJSON); err != nil {
			log.Printf("[import] %v", err)
		} else {
			log.Printf("[import] imported rule_devices from %s", cfg.rulesJSON)
		}
	}
	if db.countDevices() == 0 {
		seedDefaultDevices(db)
		seedHiotoDevices(db)
		seedHiotoRules(db)
		seedLightRules(db)
		log.Printf("[seed] seeded default devices + rules")
	}

	// Load devices + rules from SQLite.
	devices, err := db.listDevices()
	if err != nil {
		log.Fatalf("list devices: %v", err)
	}
	ruleRows, err := db.listRuleDevices()
	if err != nil {
		log.Fatalf("list rules: %v", err)
	}

	st := newState()
	// register device kinds/types from the DB + build an in-memory registry map
	// (avoids a SQLite SELECT per message).
	actType := map[string]int{}
	deviceMap := map[string]Device{}
	for _, dev := range devices {
		deviceMap[dev.GUID] = dev
		if dev.Kind == KindActuator || dev.Kind == KindHybrid {
			actType[dev.GUID] = numericType(dev.Type)
			st.putActType(dev.GUID, numericType(dev.Type))
		}
	}
	// load rules: rule_devices -> level rules + advanced threshold rules
	for _, r := range rulesFromRuleDevices(ruleRows) {
		st.putRule(r)
	}
	for _, r := range advancedRules() {
		st.putRule(r)
	}
	loadTimerRules(db, st)

	// Incoming-message handler (runs on the broker's consume goroutine).
	handle := func(d amqp.Delivery) {
		// HIOTO-topic messages use a plain {guid, value, ...} payload,
		// not the V4 envelope.
		if isHiotoRoutingKey(d.RoutingKey) {
			handleHioto(d.RoutingKey, d.Body, st, db, deviceMap)
			return
		}
		parts := strings.Split(d.RoutingKey, ".")
		var env envelope
		if json.Unmarshal(d.Body, &env) != nil {
			return
		}
		switch {
		case d.RoutingKey == "home.config.rules":
			var rm ruleMsg
			if json.Unmarshal(env.Payload, &rm) == nil {
				if rm.Action == "add" && rm.Rule.Name != "" {
					st.putRule(rm.Rule)
				} else if rm.Action == "remove" {
					st.delRule(rm.Name)
				}
			}
		case len(parts) == 5 && parts[1] == "sensor" && parts[4] == "state":
			var p statePayload
			if json.Unmarshal(env.Payload, &p) == nil {
				v := 0.0
				if len(p.DigitalValue) > 0 {
					v = float64(p.DigitalValue[0])
				} else if len(p.AnalogValue) > 0 {
					v = p.AnalogValue[0]
				}

				st.putSensor(p.SerialNumber, v)
				kickRules()
				recordTelemetry(p.SerialNumber, deviceMap[p.SerialNumber].Name, "value", v)
				_ = db.touchDevice(p.SerialNumber)
			}
		case len(parts) == 5 && parts[1] == "actuator" && parts[4] == "state":
			var p statePayload
			if json.Unmarshal(env.Payload, &p) == nil {
				if t, err := strconv.Atoi(parts[2]); err == nil {
					st.putActType(p.SerialNumber, t)
				}
				_ = db.touchDevice(p.SerialNumber)
			}
		case len(parts) == 5 && parts[1] == "actuator" && parts[4] == "override":
			var p overridePayload
			if json.Unmarshal(env.Payload, &p) == nil {
				st.putOverride(p.SerialNumber, p.Override)
			}
		}
	}

	// Connect to RabbitMQ (reconnects automatically if the broker restarts).
	binds := []string{"home.sensor.*.*.state", "home.actuator.*.*.state", "home.actuator.*.*.override", "home.config.rules"}
	binds = append(binds, hiotoTopicBinds...)
	brk := newBroker(cfg, binds, handle)
	brk.connect()

	log.Printf("home-controller v4m started: plaintext rule engine + SQLite, period=%dms, %d devices, %d rules (%d from rule_devices)", cfg.periodMS, len(devices), len(st.rules), len(ruleRows))

	publish := func(actuator string, val float64) {
		if isHiotoDevice(actuator) {
			body := hiotoCommand(actuator, val)
			if err := brk.publish("Aktuator", "text/plain", body); err != nil {
				log.Printf("[out] publish error: %v", err)
				return
			}
			recordTelemetry(actuator, deviceMap[actuator].Name, "cmd", val)
			// Devices don't publish feedback, so reflect the commanded state live.
			if dev, ok := deviceMap[actuator]; ok {
				recordLive(actuator, dev.Kind, numericType(dev.Type), val, dev.Name, dev.Type)
				_ = db.updateDeviceStatus(actuator, floatStr(val))
			}
			return
		}
		typ := st.actType(actuator)
		if typ == 0 {
			typ = 1
		}
		var payload []byte
		if typ == 2 {
			payload, _ = json.Marshal(map[string][]float64{"analog_value": {val}})
		} else {
			payload, _ = json.Marshal(map[string][]int{"digital_value": {int(val)}})
		}
		body, _ := json.Marshal(envelope{MsgID: newUUID(), TS: time.Now(), Source: "ctrl-v4m", MessageClass: "cmd", Payload: payload})
		routing := fmt.Sprintf("home.actuator.%d.%s.cmd", typ, actuator)
		if err := brk.publish(routing, "application/json", body); err != nil {
			log.Printf("[out] publish error: %v", err)
			return
		}
		recordTelemetry(actuator, deviceMap[actuator].Name, "cmd", val)
	}

	// HIOTO-compatible REST API for the existing Android app (separate port).
	startHiotoAPI(db, st, cfg, publish, deviceMap)

	// Device-management API + embedded web UI (manager/dashboard) on :8081.
	startHTTP(db, st, cfg, publish, deviceMap)

	// Periodic state sync: re-send each actuator's authoritative state so it
	// stays aligned with the controller ("1 extra command" per device). Lamps
	// don't publish feedback, so this keeps them following the correct state.
	go func() {
		t := time.NewTicker(30 * time.Second)
		defer t.Stop()
		for range t.C {
			devs, err := db.listDevices()
			if err != nil {
				continue
			}
			for _, d := range devs {
				if d.Kind != KindActuator && d.Kind != KindHybrid {
					continue
				}
				if d.Status == "" {
					continue
				}
				publish(d.GUID, parseRuleValue(d.Status))
			}
		}
	}()

	ticker := time.NewTicker(time.Duration(cfg.periodMS) * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
		case <-sensorKick:
		}
		now := time.Now()
		st.mu.RLock()
		rules := make([]rule, 0, len(st.rules))
		for _, r := range st.rules {
			rules = append(rules, r)
		}
		st.mu.RUnlock()

		sort.Slice(rules, func(i, j int) bool {
			if rules[i].Priority != rules[j].Priority {
				return rules[i].Priority > rules[j].Priority
			}
			return rules[i].Name < rules[j].Name
		})

		commanded := map[string]bool{}
		emit := func(a action) {
			if st.override(a.Actuator) || commanded[a.Actuator] {
				return
			}
			publish(a.Actuator, a.Value)
			commanded[a.Actuator] = true
		}

		for _, r := range rules {
			st.mu.RLock()
			run := st.runs[r.Name]
			st.mu.RUnlock()
			if run == nil {
				continue
			}
			if r.Mode == "trigger" {
				raw, ok := rawLevel(now, r.When, st.sensor)
				if ok && raw && !run.raw {
					for _, a := range r.Then {
						emit(a)
					}
					run.firedAt = now
				}
				// duration timer: revert (Else) once ForMinutes has elapsed
				if r.When.ForMinutes > 0 && !run.firedAt.IsZero() && now.Sub(run.firedAt) >= time.Duration(r.When.ForMinutes)*time.Minute {
					for _, a := range r.Else {
						emit(a)
					}
					run.firedAt = time.Time{}
				}
				run.raw = raw
				continue
			}
			if r.When.Type == "time" {
				out, _ := rawLevel(now, r.When, st.sensor)
				acts := r.Else
				if out {
					acts = r.Then
				}
				for _, a := range acts {
					emit(a)
				}
				continue
			}
			v, ok := st.sensor(r.When.Sensor)
			if !ok {
				continue
			}
			out := evalLevel(r.When, v, run)
			acts := r.Else
			if out {
				acts = r.Then
			}
			for _, a := range acts {
				emit(a)
			}
		}
	}
}
