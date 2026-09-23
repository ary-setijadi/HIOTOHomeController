package main

import (
	"fmt"
	"log"
	"net/url"
	"sync"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"
)

// broker wraps a reconnectable AMQP connection to RabbitMQ. If the connection
// drops (e.g. RabbitMQ is restarted), it reconnects and re-declares the
// exchange, queue, bindings, and consumer automatically.
type broker struct {
	mu     sync.Mutex
	conn   *amqp.Connection
	ch     *amqp.Channel
	cfg    config
	binds  []string
	handle func(amqp.Delivery)
}

func newBroker(cfg config, binds []string, handle func(amqp.Delivery)) *broker {
	return &broker{cfg: cfg, binds: binds, handle: handle}
}

func (b *broker) url() string {
	u := &url.URL{Scheme: "amqp", Host: fmt.Sprintf("%s:%d", b.cfg.host, b.cfg.port), Path: b.cfg.vhost}
	u.User = url.UserPassword(b.cfg.user, b.cfg.password)
	// Vhosts with a leading slash (e.g. "/smarthome") must be %2F-encoded in
	// the AMQP URI or amqp091-go parses them as "smarthome" (no slash).
	if b.cfg.vhost != "" && b.cfg.vhost != "/" {
		u.RawPath = url.PathEscape(b.cfg.vhost)
	}
	return u.String()
}

// connect retries until a connection is established (used both at startup and
// on reconnection after a drop).
func (b *broker) connect() {
	for {
		if err := b.connectOnce(); err != nil {
			log.Printf("[amqp] connect failed: %v — retrying in 2s", err)
			time.Sleep(2 * time.Second)
			continue
		}
		return
	}
}

func (b *broker) connectOnce() error {
	conn, err := amqp.Dial(b.url())
	if err != nil {
		return err
	}
	ch, err := conn.Channel()
	if err != nil {
		_ = conn.Close()
		return err
	}
	if err := ch.ExchangeDeclare(b.cfg.exchange, "topic", true, false, false, false, nil); err != nil {
		_ = ch.Close()
		_ = conn.Close()
		return err
	}
	// Non-durable + exclusive + auto-delete: when the controller disconnects,
	// the queue is deleted and un-consumed messages are lost (not buffered).
	q, err := ch.QueueDeclare("", false, true, true, false, nil)
	if err != nil {
		_ = ch.Close()
		_ = conn.Close()
		return err
	}
	for _, bd := range b.binds {
		if err := ch.QueueBind(q.Name, bd, b.cfg.exchange, false, nil); err != nil {
			_ = ch.Close()
			_ = conn.Close()
			return err
		}
	}
	msgs, err := ch.Consume(q.Name, "home-controller-v4m", true, false, false, false, nil)
	if err != nil {
		_ = ch.Close()
		_ = conn.Close()
		return err
	}

	b.mu.Lock()
	b.conn = conn
	b.ch = ch
	b.mu.Unlock()

	go func() {
		for d := range msgs {
			b.handle(d)
		}
	}()
	go b.watch(conn, ch)

	log.Printf("[amqp] connected to %s:%d", b.cfg.host, b.cfg.port)
	return nil
}

// watch reconnects when the connection drops.
func (b *broker) watch(conn *amqp.Connection, ch *amqp.Channel) {
	closed := conn.NotifyClose(make(chan *amqp.Error, 1))
	<-closed
	log.Printf("[amqp] connection closed — reconnecting")
	b.mu.Lock()
	b.conn = nil
	b.ch = nil
	b.mu.Unlock()
	b.connect()
}

// publish sends a message on the current channel (thread-safe).
func (b *broker) publish(routingKey, contentType string, body []byte) error {
	b.mu.Lock()
	ch := b.ch
	b.mu.Unlock()
	if ch == nil {
		return fmt.Errorf("amqp not connected")
	}
	return ch.Publish(b.cfg.exchange, routingKey, false, false, amqp.Publishing{
		ContentType: contentType, DeliveryMode: amqp.Transient, Body: body,
	})
}
