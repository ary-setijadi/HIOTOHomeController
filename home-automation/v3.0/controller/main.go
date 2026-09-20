// Command controller-v3 is the v3.0 home-automation controller (runs on the
// Orange Pi). It is a rule engine over DI/DO and analog devices:
//
//   rule = { name, when{sensor,op,threshold,hysteresis,min_duration_ms}, then[], else[] }
//
// Every period-ms it evaluates each rule against cached sensor values and
// publishes digital_value or analog_value commands to the mapped actuators,
// skipping any actuator under manual override.
package main

import (
	"crypto/rand"
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
	flag.IntVar(&c.port, "port", envIntOr("AMQP_PORT", 5672), "AMQP port")
	flag.StringVar(&c.vhost, "vhost", envOr("AMQP_VHOST", "/"), "vhost")
	flag.StringVar(&c.user, "user", envOr("AMQP_USER", "admin"), "username")
	flag.StringVar(&c.password, "password", envOr("AMQP_PASS", "123456Aa!"), "password")
	flag.StringVar(&c.exchange, "exchange", envOr("EXCHANGE", "home.automation"), "topic exchange")
	flag.IntVar(&c.periodMS, "period-ms", envIntOr("PERIOD_MS", 1000), "control loop cadence")
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
	Sensor        string  `json:"sensor"`
	Op            string  `json:"op"` // == != > < >= <=
	Threshold     float64 `json:"threshold"`
	Hysteresis    float64 `json:"hysteresis"`
	MinDurationMs int     `json:"min_duration_ms"`
}

type action struct {
	Actuator string  `json:"actuator"`
	Value    float64 `json:"value"`
}

type rule struct {
	Name     string    `json:"name"`
	Priority int       `json:"priority"`
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
// State
// ---------------------------------------------------------------------------
type ruleRun struct {
	out      bool
	raw      bool
	rawSince time.Time
}

type state struct {
	mu        sync.RWMutex
	sensors   map[string]float64 // serial -> value (digital 0/1 or analog float)
	overrides map[string]bool    // actuator serial -> override
	actTypes  map[string]int     // actuator serial -> device_type
	rules     map[string]rule    // name -> rule
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

func (s *state) putSensor(serial string, v float64) { s.mu.Lock(); s.sensors[serial] = v; s.mu.Unlock() }
func (s *state) putOverride(serial string, ov bool) { s.mu.Lock(); s.overrides[serial] = ov; s.mu.Unlock() }
func (s *state) putActType(serial string, t int)    { s.mu.Lock(); s.actTypes[serial] = t; s.mu.Unlock() }
func (s *state) putRule(r rule) {
	s.mu.Lock()
	s.rules[r.Name] = r
	if _, ok := s.runs[r.Name]; !ok {
		s.runs[r.Name] = &ruleRun{}
	}
	s.mu.Unlock()
}
func (s *state) delRule(name string) { s.mu.Lock(); delete(s.rules, name); delete(s.runs, name); s.mu.Unlock() }

func (s *state) sensor(serial string) (float64, bool) { s.mu.RLock(); v, ok := s.sensors[serial]; s.mu.RUnlock(); return v, ok }
func (s *state) override(serial string) bool          { s.mu.RLock(); ov := s.overrides[serial]; s.mu.RUnlock(); return ov }
func (s *state) actType(serial string) int            { s.mu.RLock(); t := s.actTypes[serial]; s.mu.RUnlock(); return t }

// ---------------------------------------------------------------------------
// Rule evaluation (hysteresis + debounce)
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

// evalRule runs the per-rule state machine and returns the current output.
func evalRule(c condition, v float64, run *ruleRun) bool {
	now := time.Now()

	// State-dependent threshold = hysteresis.
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

	// Debounce: raw must be stable for min_duration_ms.
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
// Default rules (house: 2 parents + 4 children)
// ---------------------------------------------------------------------------
func mkAction(actuator string, value float64) action { return action{Actuator: actuator, Value: value} }

func defaultRules() []rule {
	var r []rule
	// R1: switch -> lamp (per room).
	switches := []string{"SNS-SW-001", "SNS-SW-002", "SNS-SW-003", "SNS-SW-004", "SNS-SW-005", "SNS-SW-006"}
	lamps := []string{"ACT-LMP-001", "ACT-LMP-002", "ACT-LMP-003", "ACT-LMP-004", "ACT-LMP-005", "ACT-LMP-006"}
	for i := range switches {
		r = append(r, rule{
			Name: "light-" + switches[i],
			When: condition{Sensor: switches[i], Op: "==", Threshold: 1},
			Then: []action{mkAction(lamps[i], 1)},
			Else: []action{mkAction(lamps[i], 0)},
		})
	}
	// R2: water pump from flow (analog, hysteresis + debounce).
	r = append(r, rule{
		Name: "water-pump",
		When: condition{Sensor: "SNS-FLW-001", Op: "<", Threshold: 2.0, Hysteresis: 1.0, MinDurationMs: 5000},
		Then: []action{mkAction("ACT-PMP-001", 1)},
		Else: []action{mkAction("ACT-PMP-001", 0)},
	})
	// R3: AC cooling (analog, hysteresis).
	for _, ac := range []struct{ tmp, ac string }{
		{"SNS-TMP-001", "ACT-AC-001"}, {"SNS-TMP-002", "ACT-AC-002"},
		{"SNS-TMP-003", "ACT-AC-003"}, {"SNS-TMP-004", "ACT-AC-004"},
	} {
		r = append(r, rule{
			Name: "ac-" + ac.tmp,
			When: condition{Sensor: ac.tmp, Op: ">", Threshold: 26.0, Hysteresis: 1.5},
			Then: []action{mkAction(ac.ac, 0.8)},
			Else: []action{mkAction(ac.ac, 0.1)},
		})
	}
	// R4: air purifier (analog, hysteresis).
	r = append(r, rule{
		Name: "air-purifier",
		When: condition{Sensor: "SNS-AIR-001", Op: ">", Threshold: 100, Hysteresis: 20},
		Then: []action{mkAction("ACT-APR-001", 1.0)},
		Else: []action{mkAction("ACT-APR-001", 0.3)},
	})
	// R5: master off (compounding across many actuators).
	r = append(r, rule{
		Name:     "master-off",
		Priority: 100,
		When:     condition{Sensor: "SNS-SW-007", Op: "==", Threshold: 1},
		Then: []action{
			mkAction("ACT-LMP-001", 0), mkAction("ACT-LMP-002", 0), mkAction("ACT-LMP-003", 0),
			mkAction("ACT-LMP-004", 0), mkAction("ACT-LMP-005", 0), mkAction("ACT-LMP-006", 0),
			mkAction("ACT-PMP-001", 0),
		},
	})
	return r
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
func main() {
	cfg := parseConfig()

	u := &url.URL{Scheme: "amqp", Host: fmt.Sprintf("%s:%d", cfg.host, cfg.port), Path: cfg.vhost}
	u.User = url.UserPassword(cfg.user, cfg.password)
	conn, err := amqp.Dial(u.String())
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

	queue := "q.controller.v3.ctrl-0001"
	if _, err := ch.QueueDeclare(queue, true, false, false, false, nil); err != nil {
		log.Fatalf("queue: %v", err)
	}
	for _, b := range []string{"home.sensor.*.*.state", "home.actuator.*.*.override", "home.config.rules"} {
		if err := ch.QueueBind(queue, b, cfg.exchange, false, nil); err != nil {
			log.Fatalf("bind %s: %v", b, err)
		}
	}

	msgs, err := ch.Consume(queue, "home-controller-v3", false, false, false, false, nil)
	if err != nil {
		log.Fatalf("consume: %v", err)
	}

	st := newState()
	// Seed actuator types from the inventory.
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
						log.Printf("[rule] add %q (%d then / %d else)", rm.Rule.Name, len(rm.Rule.Then), len(rm.Rule.Else))
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

	log.Printf("home-controller v3.0 started: rule engine, period=%dms, %d default rules", cfg.periodMS, len(st.rules))

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
		st.mu.RLock()
		rules := make([]rule, 0, len(st.rules))
		for _, r := range st.rules {
			rules = append(rules, r)
		}
		st.mu.RUnlock()

		// Priority order: higher priority first; ties broken by name.
		sort.Slice(rules, func(i, j int) bool {
			if rules[i].Priority != rules[j].Priority {
				return rules[i].Priority > rules[j].Priority
			}
			return rules[i].Name < rules[j].Name
		})

		commanded := make(map[string]bool) // actuators already set by a higher-priority rule this tick
		for _, r := range rules {
			st.mu.RLock()
			run := st.runs[r.Name]
			st.mu.RUnlock()
			if run == nil {
				continue
			}
			v, ok := st.sensor(r.When.Sensor)
			if !ok {
				continue // no sensor data yet
			}
			out := evalRule(r.When, v, run)
			acts := r.Else
			if out {
				acts = r.Then
			}
			for _, a := range acts {
				if st.override(a.Actuator) || commanded[a.Actuator] {
					continue // manual override wins; lower-priority rule is superseded
				}
				publish(a.Actuator, a.Value)
				commanded[a.Actuator] = true
			}
		}
	}
}
