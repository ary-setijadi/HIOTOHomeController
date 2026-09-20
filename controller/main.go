package main

import (
	"encoding/json"
	"flag"
	"log"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

// Config holds all controller settings (flags with env-var fallbacks).
type Config struct {
	MQTTHost string
	MQTTPort int
	AMQPHost string
	AMQPPort int
	VHost    string
	User     string
	Password string
	Exchange string

	MQTTTelemetryTopic string
	AMQPBindingKey     string

	BroadcastEnabled   bool
	BroadcastMQTTTopic string
	BroadcastAMQPKey   string

	PerDeviceEnabled bool
	MQTTCommandTopic string
	AMQPCommandKey   string

	PeriodSeconds int
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

func parseConfig() Config {
	var c Config
	flag.StringVar(&c.MQTTHost, "mqtt-host", envOr("MQTT_HOST", "192.168.137.44"), "MQTT broker host")
	flag.IntVar(&c.MQTTPort, "mqtt-port", envIntOr("MQTT_PORT", 1883), "MQTT broker port")
	flag.StringVar(&c.AMQPHost, "amqp-host", envOr("AMQP_HOST", "192.168.137.44"), "AMQP broker host")
	flag.IntVar(&c.AMQPPort, "amqp-port", envIntOr("AMQP_PORT", 5672), "AMQP broker port")
	flag.StringVar(&c.VHost, "vhost", envOr("AMQP_VHOST", "/"), "AMQP virtual host")
	flag.StringVar(&c.User, "user", envOr("AMQP_USER", "admin"), "broker username")
	flag.StringVar(&c.Password, "password", envOr("AMQP_PASS", "123456Aa!"), "broker password")
	flag.StringVar(&c.Exchange, "exchange", envOr("EXCHANGE", "iot"), "AMQP topic exchange")

	flag.StringVar(&c.MQTTTelemetryTopic, "mqtt-telemetry-topic", envOr("MQTT_TELEMETRY_TOPIC", "iot/+/telemetry"), "MQTT telemetry subscription topic")
	flag.StringVar(&c.AMQPBindingKey, "amqp-binding-key", envOr("AMQP_BINDING_KEY", "iot.*.telemetry"), "AMQP telemetry binding key")

	flag.BoolVar(&c.BroadcastEnabled, "broadcast", true, "publish the aggregate summary to a broadcast topic/key")
	flag.StringVar(&c.BroadcastMQTTTopic, "broadcast-mqtt-topic", envOr("BROADCAST_MQTT_TOPIC", "iot/broadcast"), "MQTT broadcast topic")
	flag.StringVar(&c.BroadcastAMQPKey, "broadcast-amqp-key", envOr("BROADCAST_AMQP_KEY", "iot.broadcast"), "AMQP broadcast routing key")

	flag.BoolVar(&c.PerDeviceEnabled, "perdevice", true, "send a per-device note to each device's command channel")
	flag.StringVar(&c.MQTTCommandTopic, "mqtt-command-topic", envOr("MQTT_COMMAND_TOPIC", "iot/{id}/commands"), "MQTT command topic template")
	flag.StringVar(&c.AMQPCommandKey, "amqp-command-key", envOr("AMQP_COMMAND_KEY", "iot.{id}.commands"), "AMQP command routing key template")

	flag.IntVar(&c.PeriodSeconds, "period", envIntOr("PERIOD", 10), "processing period T in seconds")
	flag.Parse()
	return c
}

// Controller coordinates the store, the two broker connections, and the ticker.
type Controller struct {
	cfg   Config
	store *Store
	mqtt  mqtt.Client

	amqpMu sync.Mutex
	amqp   *AMQPHandle
}

func (c *Controller) setAMQP(h *AMQPHandle) {
	c.amqpMu.Lock()
	c.amqp = h
	c.amqpMu.Unlock()
}

func (c *Controller) publishAMQP(key string, payload []byte) {
	c.amqpMu.Lock()
	h := c.amqp
	c.amqpMu.Unlock()
	if h == nil {
		log.Printf("amqp: not connected; skipping publish to %q", key)
		return
	}
	h.publish(key, payload)
}

// runAMQP keeps the AMQP connection alive, reconnecting on failure.
func (c *Controller) runAMQP() {
	for {
		h, err := connectAMQP(c.cfg, c.store)
		if err != nil {
			log.Printf("amqp: %v; retrying in 3s...", err)
			time.Sleep(3 * time.Second)
			continue
		}
		c.setAMQP(h)
		<-h.Done()
		log.Printf("amqp connection closed; reconnecting...")
		c.setAMQP(nil)
	}
}

// process runs every T seconds: aggregate telemetry, then publish results.
func (c *Controller) process() {
	snapshot := c.store.Snapshot()
	summary := ComputeSummary(snapshot)
	payload, _ := json.Marshal(summary)

	log.Printf("=== SUMMARY: %d device(s) | avgTemp=%.2f | avgHum=%.2f | minBat=%.2f | maxBat=%.2f ===",
		summary.DeviceCount, summary.AvgTemperature, summary.AvgHumidity, summary.MinBattery, summary.MaxBattery)

	if c.cfg.BroadcastEnabled {
		publishMQTT(c.mqtt, c.cfg.BroadcastMQTTTopic, payload)
		c.publishAMQP(c.cfg.BroadcastAMQPKey, payload)
	}

	if c.cfg.PerDeviceEnabled {
		for _, d := range summary.Devices {
			note := DeviceNote{
				Type:            "controller-note",
				Ts:              time.Now(),
				Device:          d.Device,
				AvgTemperature:  summary.AvgTemperature,
				AvgHumidity:     summary.AvgHumidity,
				YourTemperature: d.Temperature,
				YourHumidity:    d.Humidity,
				YourBattery:     d.Battery,
			}
			np, _ := json.Marshal(note)
			mqttTopic := strings.ReplaceAll(c.cfg.MQTTCommandTopic, "{id}", d.Device)
			amqpKey := strings.ReplaceAll(c.cfg.AMQPCommandKey, "{id}", d.Device)
			publishMQTT(c.mqtt, mqttTopic, np)
			c.publishAMQP(amqpKey, np)
		}
	}
}

func main() {
	cfg := parseConfig()
	store := NewStore()
	ctrl := &Controller{cfg: cfg, store: store}

	mqttClient, err := connectMQTT(cfg, store)
	if err != nil {
		log.Fatalf("mqtt: %v", err)
	}
	ctrl.mqtt = mqttClient

	go ctrl.runAMQP()

	log.Printf("controller started | period=%ds | mqtt=%s:%d topic=%q | amqp=%s:%d exchange=%q key=%q",
		cfg.PeriodSeconds, cfg.MQTTHost, cfg.MQTTPort, cfg.MQTTTelemetryTopic,
		cfg.AMQPHost, cfg.AMQPPort, cfg.Exchange, cfg.AMQPBindingKey)

	ticker := time.NewTicker(time.Duration(cfg.PeriodSeconds) * time.Second)
	defer ticker.Stop()

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)

	for {
		select {
		case <-ticker.C:
			ctrl.process()
		case <-sig:
			log.Println("shutting down...")
			return
		}
	}
}
