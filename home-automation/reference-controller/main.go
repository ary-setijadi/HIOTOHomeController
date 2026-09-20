// Command reference-controller is a minimal AMQP controller that implements
// algorithm_id = "switch-lamp": turn the lamp actuator on when the bound
// switch sensor's digital_value[0] is 1.
//
// It validates the design's control loop end-to-end:
//   AMQP queue bound to home.sensor.*.<sensor>.state  (event-driven ingestion)
//   -> cache latest value (ack immediately)
//   -> every period_ms, run the algorithm on cached values
//   -> publish a cmd envelope to home.actuator.<type>.<actuator>.cmd
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
	"sync"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"
)

// ---------------------------------------------------------------------------
// Configuration (flags with env-var fallbacks)
// ---------------------------------------------------------------------------
type config struct {
	host          string
	port          int
	vhost         string
	user          string
	password      string
	exchange      string
	controllerID  string
	periodMS      int
	sensorSerial  string
	actuatorSerial string
	actuatorType  int
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
	flag.StringVar(&c.host, "host", envOr("AMQP_HOST", "192.168.137.44"), "AMQP host")
	flag.IntVar(&c.port, "port", envIntOr("AMQP_PORT", 5672), "AMQP port")
	flag.StringVar(&c.vhost, "vhost", envOr("AMQP_VHOST", "/"), "AMQP vhost (design uses /home-site1)")
	flag.StringVar(&c.user, "user", envOr("AMQP_USER", "admin"), "username")
	flag.StringVar(&c.password, "password", envOr("AMQP_PASS", "123456Aa!"), "password")
	flag.StringVar(&c.exchange, "exchange", envOr("EXCHANGE", "home.automation"), "topic exchange")
	flag.StringVar(&c.controllerID, "controller-id", envOr("CONTROLLER_ID", "ctrl-0001"), "controller serial/id")
	flag.IntVar(&c.periodMS, "period-ms", envIntOr("PERIOD_MS", 1000), "control loop cadence")
	flag.StringVar(&c.sensorSerial, "sensor", envOr("SENSOR_SERIAL", "SNS-SW-0001"), "bound switch sensor serial")
	flag.StringVar(&c.actuatorSerial, "actuator", envOr("ACTUATOR_SERIAL", "ACT-LMP-0002"), "bound lamp actuator serial")
	flag.IntVar(&c.actuatorType, "actuator-type", envIntOr("ACTUATOR_TYPE", 1), "actuator device_type")
	flag.Parse()
	return c
}

// ---------------------------------------------------------------------------
// Wire types (per DESIGN.md §6/§7)
// ---------------------------------------------------------------------------
type envelope struct {
	MsgID         string          `json:"msg_id"`
	TS            time.Time       `json:"ts"`
	Source        string          `json:"source"`
	MessageClass  string          `json:"message_class"`
	CorrelationID string          `json:"correlation_id,omitempty"`
	Payload       json.RawMessage `json:"payload"`
}

type statePayload struct {
	SerialNumber string    `json:"serial_number"`
	DigitalValue []int     `json:"digital_value"`
	AnalogValue  []float64 `json:"analog_value"`
}

type cmdPayload struct {
	DigitalValue []int     `json:"digital_value,omitempty"`
	AnalogValue  []float64 `json:"analog_value,omitempty"`
}

func newUUID() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	b[6] = (b[6] & 0x0f) | 0x40
	b[8] = (b[8] & 0x3f) | 0x80
	return fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16])
}

// ---------------------------------------------------------------------------
// Latest-value cache (event-driven ingestion, timer-gated evaluation)
// ---------------------------------------------------------------------------
type store struct {
	mu     sync.RWMutex
	latest map[string]statePayload
}

func newStore() *store { return &store{latest: make(map[string]statePayload)} }

func (s *store) put(k string, v statePayload) {
	s.mu.Lock()
	s.latest[k] = v
	s.mu.Unlock()
}

func (s *store) get(k string) (statePayload, bool) {
	s.mu.RLock()
	v, ok := s.latest[k]
	s.mu.RUnlock()
	return v, ok
}

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

	queue := fmt.Sprintf("q.controller.%s", cfg.controllerID)
	if _, err := ch.QueueDeclare(queue, true, false, false, false, nil); err != nil {
		log.Fatalf("queue declare: %v", err)
	}
	sensorBinding := fmt.Sprintf("home.sensor.*.%s.state", cfg.sensorSerial)
	if err := ch.QueueBind(queue, sensorBinding, cfg.exchange, false, nil); err != nil {
		log.Fatalf("queue bind: %v", err)
	}

	msgs, err := ch.Consume(queue, cfg.controllerID, false, false, false, false, nil)
	if err != nil {
		log.Fatalf("consume: %v", err)
	}

	st := newStore()

	// Event-driven ingestion: cache then ack.
	go func() {
		for d := range msgs {
			var env envelope
			if err := json.Unmarshal(d.Body, &env); err != nil {
				log.Printf("bad envelope: %v", err)
				d.Nack(false, false)
				continue
			}
			var p statePayload
			if err := json.Unmarshal(env.Payload, &p); err != nil {
				log.Printf("bad state payload: %v", err)
				d.Nack(false, false)
				continue
			}
			st.put(p.SerialNumber, p)
			log.Printf("[in]  %s state: digital=%v analog=%v", p.SerialNumber, p.DigitalValue, p.AnalogValue)
			d.Ack(false)
		}
	}()

	log.Printf("reference-controller %q started: algorithm=switch-lamp period=%dms", cfg.controllerID, cfg.periodMS)
	log.Printf("  queue=%s bound to %s on exchange %s", queue, sensorBinding, cfg.exchange)

	// Timer-gated evaluation: run the algorithm every period_ms on cached data.
	ticker := time.NewTicker(time.Duration(cfg.periodMS) * time.Millisecond)
	defer ticker.Stop()

	actuatorRouting := fmt.Sprintf("home.actuator.%d.%s.cmd", cfg.actuatorType, cfg.actuatorSerial)

	for range ticker.C {
		sw, ok := st.get(cfg.sensorSerial)
		if !ok {
			log.Printf("[eval] no cached state for %s yet; skipping", cfg.sensorSerial)
			continue
		}

		// algorithm "switch-lamp": lamp = switch.digital_value[0]
		target := 0
		if len(sw.DigitalValue) > 0 {
			target = sw.DigitalValue[0]
		}

		payload, _ := json.Marshal(cmdPayload{DigitalValue: []int{target}})
		env := envelope{
			MsgID:        newUUID(),
			TS:           time.Now(),
			Source:       cfg.controllerID,
			MessageClass: "cmd",
			Payload:      payload,
		}
		body, _ := json.Marshal(env)

		if err := ch.Publish(cfg.exchange, actuatorRouting, false, false, amqp.Publishing{
			ContentType:  "application/json",
			DeliveryMode: amqp.Persistent,
			Body:         body,
		}); err != nil {
			log.Printf("[out] publish error: %v", err)
			continue
		}
		log.Printf("[out] cmd -> %s : lamp.digital_value[0]=%d", actuatorRouting, target)
	}
}
