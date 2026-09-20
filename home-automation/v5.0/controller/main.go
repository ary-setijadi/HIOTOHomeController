// Command controller-v5 is the v5.0 home-automation controller (runs on the
// Orange Pi). It is identical to v4.0 in rule-engine semantics (level +
// trigger rules, priority, hysteresis, debounce) but connects to RabbitMQ
// over AMQPS (TLS, port 5671) using a client certificate (mutual TLS).
package main

import (
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/url"
	"os"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"
)

type config struct {
	host     string
	port     int
	vhost    string
	user     string
	password string
	exchange string
	periodMS int
	caFile   string
	certFile string
	keyFile  string
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
	flag.IntVar(&c.port, "port", envIntOr("AMQP_PORT", 5671), "AMQPS port")
	flag.StringVar(&c.vhost, "vhost", envOr("AMQP_VHOST", "/"), "vhost")
	flag.StringVar(&c.user, "user", envOr("AMQP_USER", "admin"), "username")
	flag.StringVar(&c.password, "password", envOr("AMQP_PASS", "123456Aa!"), "password")
	flag.StringVar(&c.exchange, "exchange", envOr("EXCHANGE", "home.automation"), "topic exchange")
	flag.IntVar(&c.periodMS, "period-ms", envIntOr("PERIOD_MS", 1000), "control loop cadence")
	flag.StringVar(&c.caFile, "ca", envOr("TLS_CA", "/etc/rabbitmq/certs/ca.crt"), "CA certificate")
	flag.StringVar(&c.certFile, "cert", envOr("TLS_CERT", "/etc/rabbitmq/certs/controller.crt"), "client certificate")
	flag.StringVar(&c.keyFile, "key", envOr("TLS_KEY", "/etc/rabbitmq/certs/controller.key"), "client key")
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
	Type          string  `json:"type,omitempty"` // "" (sensor) | "time"
	Sensor        string  `json:"sensor,omitempty"`
	Op            string  `json:"op,omitempty"` // == != > < >= <=
	Threshold     float64 `json:"threshold,omitempty"`
	Hysteresis    float64 `json:"hysteresis,omitempty"`
	MinDurationMs int     `json:"min_duration_ms,omitempty"`
	At            string  `json:"at,omitempty"`   // "18:00" (time trigger)
	From          string  `json:"from,omitempty"` // "18:00" (time window)
	To            string  `json:"to,omitempty"`   // "22:00"
}

type action struct {
	Actuator string  `json:"actuator"`
	Value    float64 `json:"value"`
}

type rule struct {
	Name     string    `json:"name"`
	Priority int       `json:"priority"`
	Mode     string    `json:"mode,omitempty"` // "" (level) | "trigger"
	When     condition `json:"when"`
	Then     []action  `json:"then"`
	Else     []action  `json:"else"`
}

type ruleMsg struct {
	Action string `json:"action"` // add | remove
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

// ---------------------------------------------------------------------------
// Time helpers
// ---------------------------------------------------------------------------
func minutesOfDay(t time.Time) int { return t.Hour()*60 + t.Minute() }

func parseHHMM(s string) (int, error) {
	parts := strings.Split(s, ":")
	if len(parts) != 2 {
		return 0, fmt.Errorf("bad time %q", s)
	}
	h, e1 := strconv.Atoi(parts[0])
	m, e2 := strconv.Atoi(parts[1])
	if e1 != nil || e2 != nil || h < 0 || h > 23 || m < 0 || m > 59 {
		return 0, fmt.Errorf("bad time %q", s)
	}
	return h*60 + m, nil
}

// atMinute reports whether now is within the minute identified by "HH:MM".
func atMinute(now time.Time, at string) bool {
	a, err := parseHHMM(at)
	return err == nil && minutesOfDay(now) == a
}

// inWindow reports whether now is within the daily [from, to) window (handles wrap).
func inWindow(now time.Time, from, to string) bool {
	f, e1 := parseHHMM(from)
	t, e2 := parseHHMM(to)
	if e1 != nil || e2 != nil {
		return false
	}
	n := minutesOfDay(now)
	if f < t {
		return n >= f && n < t
	}
	return n >= f || n < t
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
type ruleRun struct {
	out      bool
	raw      bool
	rawSince time.Time
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
		sensors:   make(map[string]float64),
		overrides: make(map[string]bool),
		actTypes:  make(map[string]int),
		rules:     make(map[string]rule),
		runs:      make(map[string]*ruleRun),
	}
}

func (s *state) putSensor(k string, v float64) { s.mu.Lock(); s.sensors[k] = v; s.mu.Unlock() }
func (s *state) putOverride(k string, v bool)  { s.mu.Lock(); s.overrides[k] = v; s.mu.Unlock() }
func (s *state) putActType(k string, v int)    { s.mu.Lock(); s.actTypes[k] = v; s.mu.Unlock() }
func (s *state) putRule(r rule) {
	s.mu.Lock()
	s.rules[r.Name] = r
	if _, ok := s.runs[r.Name]; !ok {
		s.runs[r.Name] = &ruleRun{}
	}
	s.mu.Unlock()
}
func (s *state) delRule(n string) { s.mu.Lock(); delete(s.rules, n); delete(s.runs, n); s.mu.Unlock() }

func (s *state) sensor(k string) (float64, bool) { s.mu.RLock(); v, ok := s.sensors[k]; s.mu.RUnlock(); return v, ok }
func (s *state) override(k string) bool          { s.mu.RLock(); v := s.overrides[k]; s.mu.RUnlock(); return v }
func (s *state) actType(k string) int            { s.mu.RLock(); v := s.actTypes[k]; s.mu.RUnlock(); return v }

// ---------------------------------------------------------------------------
// Rule evaluation
// ---------------------------------------------------------------------------
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
	default:
		return false
	}
}

// rawLevel evaluates a condition to a plain boolean (no hysteresis/debounce).
func rawLevel(now time.Time, w condition, get func(string) (float64, bool)) (bool, bool) {
	if w.Type == "time" {
		switch {
		case w.At != "":
			return atMinute(now, w.At), true
		case w.From != "" || w.To != "":
			return inWindow(now, w.From, w.To), true
		default:
			return false, true
		}
	}
	v, ok := get(w.Sensor)
	if !ok {
		return false, false
	}
	return compare(v, w.Threshold, w.Op), true
}

// evalLevel runs the hysteresis/debounce state machine for sensor level rules.
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
// Default rules
// ---------------------------------------------------------------------------
func mkAction(actuator string, value float64) action { return action{Actuator: actuator, Value: value} }

func defaultRules() []rule {
	var r []rule
	sw := []string{"SNS-SW-001", "SNS-SW-002", "SNS-SW-003", "SNS-SW-004", "SNS-SW-005", "SNS-SW-006"}
	lp := []string{"ACT-LMP-001", "ACT-LMP-002", "ACT-LMP-003", "ACT-LMP-004", "ACT-LMP-005", "ACT-LMP-006"}
	for i := range sw {
		r = append(r, rule{Name: "light-" + sw[i], When: condition{Sensor: sw[i], Op: "==", Threshold: 1},
			Then: []action{mkAction(lp[i], 1)}, Else: []action{mkAction(lp[i], 0)}})
	}
	r = append(r, rule{Name: "water-pump", When: condition{Sensor: "SNS-FLW-001", Op: "<", Threshold: 2.0, Hysteresis: 1.0, MinDurationMs: 5000},
		Then: []action{mkAction("ACT-PMP-001", 1)}, Else: []action{mkAction("ACT-PMP-001", 0)}})
	for _, ac := range []struct{ t, a string }{
		{"SNS-TMP-001", "ACT-AC-001"}, {"SNS-TMP-002", "ACT-AC-002"}, {"SNS-TMP-003", "ACT-AC-003"}, {"SNS-TMP-004", "ACT-AC-004"},
	} {
		r = append(r, rule{Name: "ac-" + ac.t, When: condition{Sensor: ac.t, Op: ">", Threshold: 26.0, Hysteresis: 1.5},
			Then: []action{mkAction(ac.a, 0.8)}, Else: []action{mkAction(ac.a, 0.1)}})
	}
	r = append(r, rule{Name: "air-purifier", When: condition{Sensor: "SNS-AIR-001", Op: ">", Threshold: 100, Hysteresis: 20},
		Then: []action{mkAction("ACT-APR-001", 1.0)}, Else: []action{mkAction("ACT-APR-001", 0.3)}})
	r = append(r, rule{Name: "master-off", Priority: 100, When: condition{Sensor: "SNS-SW-007", Op: "==", Threshold: 1},
		Then: append(allOff(lp), mkAction("ACT-PMP-001", 0))})

	// v5.0 keeps the v4.0 time trigger — "at 18:00 turn all lamps ON (once)".
	r = append(r, rule{Name: "evening-on", Mode: "trigger", When: condition{Type: "time", At: "18:00"},
		Then: allOn(lp)})
	return r
}

func allOn(actuators []string) []action {
	out := make([]action, 0, len(actuators))
	for _, a := range actuators {
		out = append(out, mkAction(a, 1))
	}
	return out
}
func allOff(actuators []string) []action {
	out := make([]action, 0, len(actuators))
	for _, a := range actuators {
		out = append(out, mkAction(a, 0))
	}
	return out
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
func main() {
	cfg := parseConfig()

	// Build mutual-TLS client configuration.
	ca, err := os.ReadFile(cfg.caFile)
	if err != nil {
		log.Fatalf("read CA %s: %v", cfg.caFile, err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(ca) {
		log.Fatalf("append CA %s: no certificates parsed", cfg.caFile)
	}
	cert, err := tls.LoadX509KeyPair(cfg.certFile, cfg.keyFile)
	if err != nil {
		log.Fatalf("load keypair %s/%s: %v", cfg.certFile, cfg.keyFile, err)
	}
	tlsCfg := &tls.Config{
		RootCAs:      pool,
		Certificates: []tls.Certificate{cert},
		ServerName:   "maincontroller",
		MinVersion:   tls.VersionTLS12,
	}

	u := &url.URL{Scheme: "amqps", Host: fmt.Sprintf("%s:%d", cfg.host, cfg.port), Path: cfg.vhost}
	u.User = url.UserPassword(cfg.user, cfg.password)
	conn, err := amqp.DialTLS(u.String(), tlsCfg)
	if err != nil {
		log.Fatalf("dial: %v", err)
	}
	defer conn.Close()
	ch, err := conn.Channel()
	if err != nil {
		log.Fatalf("channel: %v", err)
	}
	if err := ch.ExchangeDeclare(cfg.exchange, "topic", true, false, false, false, nil); err != nil {
		log.Fatalf("exchange: %v", err)
	}

	queue := "q.controller.v5.ctrl-0001"
	if _, err := ch.QueueDeclare(queue, true, false, false, false, nil); err != nil {
		log.Fatalf("queue: %v", err)
	}
	for _, b := range []string{"home.sensor.*.*.state", "home.actuator.*.*.override", "home.config.rules"} {
		if err := ch.QueueBind(queue, b, cfg.exchange, false, nil); err != nil {
			log.Fatalf("bind %s: %v", b, err)
		}
	}

	msgs, err := ch.Consume(queue, "home-controller-v5", false, false, false, false, nil)
	if err != nil {
		log.Fatalf("consume: %v", err)
	}

	st := newState()
	for _, a := range []string{"ACT-LMP-001", "ACT-LMP-002", "ACT-LMP-003", "ACT-LMP-004", "ACT-LMP-005", "ACT-LMP-006", "ACT-PMP-001"} {
		st.putActType(a, 1)
	}
	for _, a := range []string{"ACT-AC-001", "ACT-AC-002", "ACT-AC-003", "ACT-AC-004", "ACT-APR-001"} {
		st.putActType(a, 2)
	}
	for _, r := range defaultRules() {
		st.putRule(r)
	}

	go func() {
		for d := range msgs {
			parts := strings.Split(d.RoutingKey, ".")
			var env envelope
			if json.Unmarshal(d.Body, &env) != nil {
				d.Ack(false)
				continue
			}
			switch {
			case d.RoutingKey == "home.config.rules":
				var rm ruleMsg
				if json.Unmarshal(env.Payload, &rm) == nil {
					if rm.Action == "add" && rm.Rule.Name != "" {
						st.putRule(rm.Rule)
						log.Printf("[rule] add %q (mode=%s)", rm.Rule.Name, rm.Rule.Mode)
					} else if rm.Action == "remove" {
						st.delRule(rm.Name)
						log.Printf("[rule] remove %q", rm.Name)
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
				}
			case len(parts) == 5 && parts[1] == "actuator" && parts[4] == "state":
				var p statePayload
				if json.Unmarshal(env.Payload, &p) == nil {
					if t, err := strconv.Atoi(parts[2]); err == nil {
						st.putActType(p.SerialNumber, t)
					}
				}
			case len(parts) == 5 && parts[1] == "actuator" && parts[4] == "override":
				var p overridePayload
				if json.Unmarshal(env.Payload, &p) == nil {
					st.putOverride(p.SerialNumber, p.Override)
					log.Printf("[in] override %s = %v", p.SerialNumber, p.Override)
				}
			}
			d.Ack(false)
		}
	}()

	log.Printf("home-controller v5.0 started: mTLS rule engine (level + trigger), period=%dms, %d default rules", cfg.periodMS, len(st.rules))

	publish := func(actuator string, val float64) {
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
		body, _ := json.Marshal(envelope{MsgID: newUUID(), TS: time.Now(), Source: "ctrl-0001", MessageClass: "cmd", Payload: payload})
		routing := fmt.Sprintf("home.actuator.%d.%s.cmd", typ, actuator)
		if err := ch.Publish(cfg.exchange, routing, false, false, amqp.Publishing{
			ContentType: "application/json", DeliveryMode: amqp.Persistent, Body: body,
		}); err != nil {
			log.Printf("[out] publish error: %v", err)
			return
		}
		log.Printf("[out] %s = %v (type %d)", actuator, val, typ)
	}

	ticker := time.NewTicker(time.Duration(cfg.periodMS) * time.Millisecond)
	defer ticker.Stop()

	for range ticker.C {
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

		commanded := make(map[string]bool)

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
				if !ok {
					continue
				}
				if raw && !run.raw {
					log.Printf("[trigger] %q fired (at %s)", r.Name, now.Format("15:04:05"))
					for _, a := range r.Then {
						emit(a)
					}
				}
				run.raw = raw
				continue
			}

			// level mode
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
