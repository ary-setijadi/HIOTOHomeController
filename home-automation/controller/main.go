// Command controller is the home-automation controller (runs on the Orange Pi).
//
// It implements algorithm_id = "switch-lamp" for 5 paired devices:
//   lamp[i] = switch[i].digital_value[0],  evaluated every period-ms.
//
// It is AMQP (backend tier): it consumes sensor `state` and actuator `override`
// messages from the shared home.automation topic exchange and publishes `cmd`
// to each lamp. A lamp under manual override is skipped (supervisory control
// wins).
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
	host      string
	port      int
	vhost     string
	user      string
	password  string
	exchange  string
	periodMS  int
	sensors   []string
	actuators []string
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
	var sensors, actuators string
	flag.StringVar(&c.host, "host", envOr("AMQP_HOST", "127.0.0.1"), "AMQP host (127.0.0.1 when running on the Pi)")
	flag.IntVar(&c.port, "port", envIntOr("AMQP_PORT", 5672), "AMQP port")
	flag.StringVar(&c.vhost, "vhost", envOr("AMQP_VHOST", "/"), "AMQP vhost")
	flag.StringVar(&c.user, "user", envOr("AMQP_USER", "admin"), "username")
	flag.StringVar(&c.password, "password", envOr("AMQP_PASS", "123456Aa!"), "password")
	flag.StringVar(&c.exchange, "exchange", envOr("EXCHANGE", "home.automation"), "topic exchange")
	flag.IntVar(&c.periodMS, "period-ms", envIntOr("PERIOD_MS", 1000), "control loop cadence")
	flag.StringVar(&sensors, "sensors", envOr("SENSORS", "SNS-SW-0001,SNS-SW-0002,SNS-SW-0003,SNS-SW-0004,SNS-SW-0005"), "comma-separated switch sensor serials")
	flag.StringVar(&actuators, "actuators", envOr("ACTUATORS", "ACT-LMP-0001,ACT-LMP-0002,ACT-LMP-0003,ACT-LMP-0004,ACT-LMP-0005"), "comma-separated lamp actuator serials")
	flag.Parse()

	c.sensors = splitCSV(sensors)
	c.actuators = splitCSV(actuators)
	return c
}

func splitCSV(s string) []string {
	var out []string
	for _, p := range strings.Split(s, ",") {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

// ---------------------------------------------------------------------------
// Wire types (DESIGN.md §6/§7)
// ---------------------------------------------------------------------------
type envelope struct {
	MsgID         string          `json:"msg_id"`
	TS            time.Time       `json:"ts"`
	Source        string          `json:"source"`
	MessageClass  string          `json:"message_class"`
	CorrelationID string          `json:"correlation_id,omitempty"`
	Payload       json.RawMessage `json:"payload"`
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

func newUUID() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

// ---------------------------------------------------------------------------
// Latest-value cache
// ---------------------------------------------------------------------------
type state struct {
	mu        sync.RWMutex
	sensors   map[string][]int  // switch serial -> digital_value
	overrides map[string]bool   // actuator serial -> manual override
}

func newState() *state {
	return &state{sensors: make(map[string][]int), overrides: make(map[string]bool)}
}

func (s *state) putSensor(serial string, dv []int) {
	s.mu.Lock()
	s.sensors[serial] = dv
	s.mu.Unlock()
}

func (s *state) putOverride(serial string, ov bool) {
	s.mu.Lock()
	s.overrides[serial] = ov
	s.mu.Unlock()
}

func (s *state) sensor(serial string) ([]int, bool) {
	s.mu.RLock()
	dv, ok := s.sensors[serial]
	s.mu.RUnlock()
	return dv, ok
}

func (s *state) override(serial string) bool {
	s.mu.RLock()
	ov := s.overrides[serial]
	s.mu.RUnlock()
	return ov
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
func main() {
	cfg := parseConfig()
	if len(cfg.sensors) != len(cfg.actuators) {
		log.Fatalf("sensors(%d) and actuators(%d) must pair 1:1", len(cfg.sensors), len(cfg.actuators))
	}

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

	queue := "q.controller.ctrl-0001"
	if _, err := ch.QueueDeclare(queue, true, false, false, false, nil); err != nil {
		log.Fatalf("queue declare: %v", err)
	}
	for _, binding := range []string{"home.sensor.*.*.state", "home.actuator.*.*.override"} {
		if err := ch.QueueBind(queue, binding, cfg.exchange, false, nil); err != nil {
			log.Fatalf("queue bind %s: %v", binding, err)
		}
	}

	msgs, err := ch.Consume(queue, "home-controller", false, false, false, false, nil)
	if err != nil {
		log.Fatalf("consume: %v", err)
	}

	st := newState()

	// Event-driven ingestion: cache then ack.
	go func() {
		for d := range msgs {
			parts := strings.Split(d.RoutingKey, ".") // home.<kind>.<type>.<serial>.<class>
			if len(parts) != 5 {
				d.Ack(false)
				continue
			}
			kind, _, class := parts[1], parts[3], parts[4]

			var env envelope
			if err := json.Unmarshal(d.Body, &env); err != nil {
				d.Ack(false)
				continue
			}
			switch {
			case kind == "sensor" && class == "state":
				var p sensorPayload
				if json.Unmarshal(env.Payload, &p) == nil {
					st.putSensor(p.SerialNumber, p.DigitalValue)
					log.Printf("[in]  sensor  %s state=%v", p.SerialNumber, p.DigitalValue)
				}
			case kind == "actuator" && class == "override":
				var p overridePayload
				if json.Unmarshal(env.Payload, &p) == nil {
					st.putOverride(p.SerialNumber, p.Override)
					log.Printf("[in]  override %s = %v", p.SerialNumber, p.Override)
				}
			}
			d.Ack(false)
		}
	}()

	log.Printf("home-controller started: algorithm=switch-lamp period=%dms exchange=%s", cfg.periodMS, cfg.exchange)
	log.Printf("  pairs: %v -> %v", cfg.sensors, cfg.actuators)

	ticker := time.NewTicker(time.Duration(cfg.periodMS) * time.Millisecond)
	defer ticker.Stop()

	for range ticker.C {
		for i, ssn := range cfg.sensors {
			asn := cfg.actuators[i]

			if st.override(asn) {
				log.Printf("[skip] %s under manual override", asn)
				continue
			}
			dv, ok := st.sensor(ssn)
			if !ok {
				log.Printf("[skip] %s: no sensor state yet", ssn)
				continue
			}
			target := 0
			if len(dv) > 0 {
				target = dv[0]
			}

			payload, _ := json.Marshal(cmdPayload{DigitalValue: []int{target}})
			body, _ := json.Marshal(envelope{
				MsgID:        newUUID(),
				TS:           time.Now(),
				Source:       "ctrl-0001",
				MessageClass: "cmd",
				Payload:      payload,
			})
			routing := fmt.Sprintf("home.actuator.1.%s.cmd", asn)
			if err := ch.Publish(cfg.exchange, routing, false, false, amqp.Publishing{
				ContentType:  "application/json",
				DeliveryMode: amqp.Persistent,
				Body:         body,
			}); err != nil {
				log.Printf("[out] publish error: %v", err)
				continue
			}
			log.Printf("[out] cmd %s = %d", asn, target)
		}
	}
}
