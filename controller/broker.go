package main

import (
	"encoding/json"
	"fmt"
	"log"
	"math/rand"
	"net/url"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
	amqp "github.com/rabbitmq/amqp091-go"
)

// ---------------------------------------------------------------------------
// MQTT
// ---------------------------------------------------------------------------

func connectMQTT(cfg Config, store *Store) (mqtt.Client, error) {
	broker := fmt.Sprintf("tcp://%s:%d", cfg.MQTTHost, cfg.MQTTPort)
	opts := mqtt.NewClientOptions().
		AddBroker(broker).
		SetClientID(fmt.Sprintf("controller-%d", rand.Intn(100000))).
		SetUsername(cfg.User).
		SetPassword(cfg.Password).
		SetAutoReconnect(true).
		SetConnectRetry(true).
		SetConnectRetryInterval(3 * time.Second).
		SetCleanSession(true)

	opts.SetOnConnectHandler(func(c mqtt.Client) {
		if tok := c.Subscribe(cfg.MQTTTelemetryTopic, 1, mqttTelemetryHandler(store)); tok.Wait() && tok.Error() != nil {
			log.Printf("mqtt: subscribe error: %v", tok.Error())
		} else {
			log.Printf("mqtt: subscribed to %q", cfg.MQTTTelemetryTopic)
		}
	})

	client := mqtt.NewClient(opts)
	if tok := client.Connect(); tok.Wait() && tok.Error() != nil {
		return nil, fmt.Errorf("connect: %w", tok.Error())
	}
	return client, nil
}

func mqttTelemetryHandler(store *Store) mqtt.MessageHandler {
	return func(c mqtt.Client, m mqtt.Message) {
		var d struct {
			Device      string  `json:"device"`
			Temperature float64 `json:"temperature"`
			Humidity    float64 `json:"humidity"`
			Battery     float64 `json:"battery"`
		}
		if err := json.Unmarshal(m.Payload(), &d); err != nil {
			log.Printf("mqtt: bad payload on %q: %v", m.Topic(), err)
			return
		}
		store.Update(DeviceReading{Device: d.Device, Temperature: d.Temperature, Humidity: d.Humidity, Battery: d.Battery, Source: "mqtt"})
		log.Printf("[mqtt] telemetry: device=%s temp=%.2f hum=%.1f bat=%.2f", d.Device, d.Temperature, d.Humidity, d.Battery)
	}
}

func publishMQTT(client mqtt.Client, topic string, payload []byte) {
	if tok := client.Publish(topic, 1, false, payload); tok.Wait() && tok.Error() != nil {
		log.Printf("mqtt: publish error: %v", tok.Error())
	} else {
		log.Printf("mqtt: published to %q", topic)
	}
}

// ---------------------------------------------------------------------------
// AMQP
// ---------------------------------------------------------------------------

// AMQPHandle wraps a live AMQP connection/channel and signals when it closes.
type AMQPHandle struct {
	conn *amqp.Connection
	ch   *amqp.Channel
	cfg  Config
	done chan struct{}
}

func (h *AMQPHandle) Done() <-chan struct{} { return h.done }

func (h *AMQPHandle) publish(key string, payload []byte) {
	err := h.ch.Publish(h.cfg.Exchange, key, false, false, amqp.Publishing{
		ContentType:  "application/json",
		Body:         payload,
		DeliveryMode: amqp.Persistent,
	})
	if err != nil {
		log.Printf("amqp: publish error: %v", err)
	} else {
		log.Printf("amqp: published to %q", key)
	}
}

func connectAMQP(cfg Config, store *Store) (*AMQPHandle, error) {
	u := &url.URL{Scheme: "amqp", Host: fmt.Sprintf("%s:%d", cfg.AMQPHost, cfg.AMQPPort), Path: cfg.VHost}
	u.User = url.UserPassword(cfg.User, cfg.Password)

	conn, err := amqp.Dial(u.String())
	if err != nil {
		return nil, fmt.Errorf("dial: %w", err)
	}
	ch, err := conn.Channel()
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("channel: %w", err)
	}
	if err := ch.ExchangeDeclare(cfg.Exchange, "topic", true, false, false, false, nil); err != nil {
		conn.Close()
		return nil, fmt.Errorf("exchange declare: %w", err)
	}
	q, err := ch.QueueDeclare("controller.telemetry", true, false, false, false, nil)
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("queue declare: %w", err)
	}
	if err := ch.QueueBind(q.Name, cfg.AMQPBindingKey, cfg.Exchange, false, nil); err != nil {
		conn.Close()
		return nil, fmt.Errorf("queue bind: %w", err)
	}
	msgs, err := ch.Consume(q.Name, "controller", false, false, false, false, nil)
	if err != nil {
		conn.Close()
		return nil, fmt.Errorf("consume: %w", err)
	}

	go func() {
		for d := range msgs {
			var dev struct {
				Device      string  `json:"device"`
				Temperature float64 `json:"temperature"`
				Humidity    float64 `json:"humidity"`
				Battery     float64 `json:"battery"`
			}
			if err := json.Unmarshal(d.Body, &dev); err != nil {
				log.Printf("amqp: bad payload: %v", err)
				d.Nack(false, false)
				continue
			}
			store.Update(DeviceReading{Device: dev.Device, Temperature: dev.Temperature, Humidity: dev.Humidity, Battery: dev.Battery, Source: "amqp"})
			log.Printf("[amqp] telemetry: device=%s temp=%.2f hum=%.1f bat=%.2f", dev.Device, dev.Temperature, dev.Humidity, dev.Battery)
			d.Ack(false)
		}
	}()

	done := make(chan struct{})
	closeCh := conn.NotifyClose(make(chan *amqp.Error))
	go func() {
		<-closeCh
		close(done)
	}()

	log.Printf("amqp: queue %q bound to exchange %q with key %q", q.Name, cfg.Exchange, cfg.AMQPBindingKey)
	return &AMQPHandle{conn: conn, ch: ch, cfg: cfg, done: done}, nil
}
