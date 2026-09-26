package main

import (
	"crypto/tls"
	"log"
	"sync"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"
)

// broker wraps a reconnectable AMQP-over-TLS connection. On drop it reconnects
// and re-declares the exchange, queue, binding, and consumer.
type broker struct {
	mu     sync.Mutex
	conn   *amqp.Connection
	ch     *amqp.Channel
	cfg    config
	tls    *tls.Config
	handle func(amqp.Delivery)
}

func newBroker(cfg config, tlsCfg *tls.Config, handle func(amqp.Delivery)) *broker {
	return &broker{cfg: cfg, tls: tlsCfg, handle: handle}
}

func (b *broker) connect() {
	for {
		if err := b.connectOnce(); err != nil {
			log.Printf("[amqp] connect failed: %v — retrying in 5s", err)
			time.Sleep(5 * time.Second)
			continue
		}
		return
	}
}

func (b *broker) connectOnce() error {
	conn, err := amqp.DialTLS(amqpURL(b.cfg), b.tls)
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
	// Exclusive + auto-delete: on disconnect the queue is deleted, so no stale
	// backlog accumulates across reconnects.
	q, err := ch.QueueDeclare("", false, true, true, false, nil)
	if err != nil {
		_ = ch.Close()
		_ = conn.Close()
		return err
	}
	// "#" binds to every routing key on the topic exchange (full mirror).
	if err := ch.QueueBind(q.Name, "#", b.cfg.exchange, false, nil); err != nil {
		_ = ch.Close()
		_ = conn.Close()
		return err
	}
	msgs, err := ch.Consume(q.Name, "agent-v4m", true, false, false, false, nil)
	if err != nil {
		_ = ch.Close()
		_ = conn.Close()
		return err
	}

	b.mu.Lock()
	b.conn, b.ch = conn, ch
	b.mu.Unlock()

	go func() {
		for d := range msgs {
			b.handle(d)
		}
	}()
	go b.watch(conn, ch)

	log.Printf("[amqp] connected to %s:%d vhost %q", b.cfg.host, b.cfg.port, b.cfg.vhost)
	return nil
}

func (b *broker) watch(conn *amqp.Connection, ch *amqp.Channel) {
	closed := conn.NotifyClose(make(chan *amqp.Error, 1))
	<-closed
	log.Printf("[amqp] connection closed — reconnecting")
	b.mu.Lock()
	b.conn, b.ch = nil, nil
	b.mu.Unlock()
	b.connect()
}
