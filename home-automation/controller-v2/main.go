// Command controller-v2 is the v2.0 home-automation controller (runs on the
// Orange Pi). It is rule-based:
//
//   rule = switch -> []{actuator, on, off}
//   every period-ms, for each rule: read switch state, then command each mapped
//   actuator to `on` (switch ON) or `off` (switch OFF), skipping any actuator
//   under manual override.
//
// Rules are registered/removed at runtime via messages on home/config/rules.
package main

import (
	"crypto/rand"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"net/url"
	"os"
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

type sensorPayload struct {
	SerialNumber string `json:"serial_number"`
	DigitalValue []int  `json:"digital_value"`
}

type overridePayload struct {
	SerialNumber string `json:"serial_number"`
	Override     bool   `json:"override"`
}

type cmdPayload struct {
	DigitalValue []int `json:"digital_value"`
}

// A rule maps one switch to several actuator targets.
type mapping struct {
	Actuator string `json:"actuator"`
	On       int    `json:"on"`
	Off      int    `json:"off"`
}

type rule struct {
	Switch   string    `json:"switch"`
	Mappings []mapping `json:"mappings"`
}

type ruleMsg struct {
	Action   string    `json:"action"` // "add" | "remove"
	Switch   string    `json:"switch"`
	Mappings []mapping `json:"mappings"`
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
type state struct {
	mu        sync.RWMutex
	sensors   map[string][]int // switch serial -> digital_value
	overrides map[string]bool  // actuator serial -> override
	rules     map[string]rule  // switch serial -> rule
}

func newState() *state {
	return &state{
		sensors:   make(map[string][]int),
		overrides: make(map[string]bool),
		rules:     make(map[string]rule),
	}
}

func (s *state) putSensor(serial string, dv []int) {
	s.mu.Lock(); s.sensors[serial] = dv; s.mu.Unlock()
}
func (s *state) putOverride(serial string, ov bool) {
	s.mu.Lock(); s.overrides[serial] = ov; s.mu.Unlock()
}
func (s *state) putRule(r rule) {
	s.mu.Lock(); s.rules[r.Switch] = r; s.mu.Unlock()
}
func (s *state) delRule(sw string) {
	s.mu.Lock(); delete(s.rules, sw); s.mu.Unlock()
}
func (s *state) sensor(serial string) ([]int, bool) {
	s.mu.RLock(); dv, ok := s.sensors[serial]; s.mu.RUnlock(); return dv, ok
}
func (s *state) override(serial string) bool {
	s.mu.RLock(); ov := s.overrides[serial]; s.mu.RUnlock(); return ov
}
func (s *state) rulesSnapshot() []rule {
	s.mu.RLock()
	out := make([]rule, 0, len(s.rules))
	for _, r := range s.rules { out = append(out, r) }
	s.mu.RUnlock()
	return out
}

// ---------------------------------------------------------------------------
// Default rules (demonstrate compounding: switch 1 drives lamps 1 AND 3)
// ---------------------------------------------------------------------------
func defaultRules() []rule {
	return []rule{
		{Switch: "SNS-SW-0001", Mappings: []mapping{{Actuator: "ACT-LMP-0001", On: 1, Off: 0}, {Actuator: "ACT-LMP-0003", On: 1, Off: 0}}},
		{Switch: "SNS-SW-0002", Mappings: []mapping{{Actuator: "ACT-LMP-0002", On: 1, Off: 0}}},
		{Switch: "SNS-SW-0003", Mappings: []mapping{{Actuator: "ACT-LMP-0005", On: 1, Off: 0}}},
		{Switch: "SNS-SW-0004", Mappings: []mapping{{Actuator: "ACT-LMP-0004", On: 1, Off: 0}}},
		// SNS-SW-0005 deliberately has no default rule — register one at runtime.
	}
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
		log.Fatalf("exchange declare: %v", err)
	}

	queue := "q.controller.v2.ctrl-0001"
	if _, err := ch.QueueDeclare(queue, true, false, false, false, nil); err != nil {
		log.Fatalf("queue declare: %v", err)
	}
	for _, binding := range []string{"home.sensor.*.*.state", "home.actuator.*.*.override", "home.config.rules"} {
		if err := ch.QueueBind(queue, binding, cfg.exchange, false, nil); err != nil {
			log.Fatalf("bind %s: %v", binding, err)
		}
	}

	msgs, err := ch.Consume(queue, "home-controller-v2", false, false, false, false, nil)
	if err != nil {
		log.Fatalf("consume: %v", err)
	}

	st := newState()
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
					if rm.Action == "add" {
						st.putRule(rule{Switch: rm.Switch, Mappings: rm.Mappings})
						log.Printf("[rule] add %s -> %d mapping(s)", rm.Switch, len(rm.Mappings))
					} else if rm.Action == "remove" {
						st.delRule(rm.Switch)
						log.Printf("[rule] remove %s", rm.Switch)
					}
				}
			case len(parts) == 5 && parts[1] == "sensor" && parts[4] == "state":
				var p sensorPayload
				if json.Unmarshal(env.Payload, &p) == nil {
					st.putSensor(p.SerialNumber, p.DigitalValue)
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

	log.Printf("home-controller v2.0 started: rule-based, period=%dms exchange=%s", cfg.periodMS, cfg.exchange)
	log.Printf("  default rules: %d", len(st.rulesSnapshot()))

	ticker := time.NewTicker(time.Duration(cfg.periodMS) * time.Millisecond)
	defer ticker.Stop()

	for range ticker.C {
		for _, r := range st.rulesSnapshot() {
			dv, ok := st.sensor(r.Switch)
			if !ok {
				continue // no sensor state yet
			}
			sw := 0
			if len(dv) > 0 {
				sw = dv[0]
			}
			for _, m := range r.Mappings {
				if st.override(m.Actuator) {
					continue // manual override wins
				}
				target := m.Off
				if sw == 1 {
					target = m.On
				}
				payload, _ := json.Marshal(cmdPayload{DigitalValue: []int{target}})
				body, _ := json.Marshal(envelope{MsgID: newUUID(), TS: time.Now(), Source: "ctrl-0001", MessageClass: "cmd", Payload: payload})
				routing := fmt.Sprintf("home.actuator.1.%s.cmd", m.Actuator)
				if err := ch.Publish(cfg.exchange, routing, false, false, amqp.Publishing{
					ContentType: "application/json", DeliveryMode: amqp.Persistent, Body: body,
				}); err != nil {
					log.Printf("[out] publish error: %v", err)
					continue
				}
				log.Printf("[out] %s(switch=%d) -> %s = %d", r.Switch, sw, m.Actuator, target)
			}
		}
	}
}
