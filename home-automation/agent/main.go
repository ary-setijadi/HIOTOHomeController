// Command agent-v4m is the off-site datacenter client for the home-automation
// broker. It connects over AMQP + mutual TLS, mirrors every message on the
// home.automation exchange to a rolling JSONL store (off-site backup / remote
// monitoring), and can publish actuator commands one-shot (remote control).
package main

import (
	"crypto/tls"
	"crypto/x509"
	"flag"
	"fmt"
	"log"
	"net/url"
	"os"
	"os/signal"
	"syscall"

	amqp "github.com/rabbitmq/amqp091-go"
)

type config struct {
	host        string
	port        int
	vhost       string
	user        string
	password    string
	ca          string
	cert        string
	key         string
	serverName  string
	exchange    string
	outDir      string
	retention   int
	publishKey  string
	publishBody string
	insecure    bool
}

func main() {
	var cfg config
	flag.StringVar(&cfg.host, "broker-host", envOr("BROKER_HOST", "192.168.1.22"), "broker host/IP")
	flag.IntVar(&cfg.port, "port", envIntOr("BROKER_PORT", 5671), "AMQP TLS port")
	flag.StringVar(&cfg.vhost, "vhost", envOr("BROKER_VHOST", "/smarthome"), "RabbitMQ vhost")
	flag.StringVar(&cfg.user, "user", envOr("BROKER_USER", "agent"), "RabbitMQ user")
	flag.StringVar(&cfg.password, "password", envOr("BROKER_PASSWORD", "Agent!23"), "RabbitMQ password")
	flag.StringVar(&cfg.ca, "ca", envOr("AGENT_CA", "ca.crt"), "CA cert PEM")
	flag.StringVar(&cfg.cert, "cert", envOr("AGENT_CERT", "agent.crt"), "client cert PEM")
	flag.StringVar(&cfg.key, "key", envOr("AGENT_KEY", "agent.key"), "client key PEM")
	flag.StringVar(&cfg.serverName, "server-name", envOr("BROKER_SNI", "maincontroller"), "TLS server name (SNI + verify)")
	flag.StringVar(&cfg.exchange, "exchange", "home.automation", "topic exchange")
	flag.StringVar(&cfg.outDir, "out-dir", envOr("AGENT_OUT", "/var/lib/agent-v4m"), "mirror output directory")
	flag.IntVar(&cfg.retention, "retention-days", 30, "delete mirror files older than N days (0 = keep forever)")
	flag.StringVar(&cfg.publishKey, "publish", "", "one-shot publish: routing key (e.g. Aktuator)")
	flag.StringVar(&cfg.publishBody, "publish-body", "", "one-shot publish: body (e.g. \"guid#0\")")
	flag.BoolVar(&cfg.insecure, "insecure", false, "skip TLS verification (test only)")
	flag.Parse()

	tlsCfg, err := buildTLS(cfg)
	if err != nil {
		log.Fatalf("tls: %v", err)
	}

	if cfg.publishKey != "" {
		if err := runPublish(cfg, tlsCfg); err != nil {
			log.Fatalf("publish: %v", err)
		}
		log.Printf("published %d bytes to routing key %q", len(cfg.publishBody), cfg.publishKey)
		return
	}

	store, err := newStore(cfg.outDir, cfg.retention)
	if err != nil {
		log.Fatalf("store: %v", err)
	}
	defer store.close()

	b := newBroker(cfg, tlsCfg, func(d amqp.Delivery) {
		store.append(d.RoutingKey, d.Body)
		log.Printf("[in] %s: %s", d.RoutingKey, truncate(string(d.Body), 120))
	})
	b.connect()

	log.Printf("agent-v4m mirroring exchange %q on %s:%d vhost %q -> %s", cfg.exchange, cfg.host, cfg.port, cfg.vhost, cfg.outDir)

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt, syscall.SIGTERM)
	<-sig
	log.Printf("shutting down")
}

// amqpURL builds an amqps:// URI. The password may contain reserved characters
// (e.g. "!"); url.UserPassword keeps them as valid userinfo. The vhost leading
// slash is %2F-encoded so it is not lost by the URI parser.
func amqpURL(cfg config) string {
	u := &url.URL{Scheme: "amqps", Host: fmt.Sprintf("%s:%d", cfg.host, cfg.port), Path: cfg.vhost}
	u.User = url.UserPassword(cfg.user, cfg.password)
	if cfg.vhost != "" && cfg.vhost != "/" {
		u.RawPath = url.PathEscape(cfg.vhost)
	}
	return u.String()
}

func buildTLS(cfg config) (*tls.Config, error) {
	caPEM, err := os.ReadFile(cfg.ca)
	if err != nil {
		return nil, fmt.Errorf("read CA %s: %w", cfg.ca, err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(caPEM) {
		return nil, fmt.Errorf("no CA certs found in %s", cfg.ca)
	}
	cert, err := tls.LoadX509KeyPair(cfg.cert, cfg.key)
	if err != nil {
		return nil, fmt.Errorf("load client cert/key: %w", err)
	}
	tc := &tls.Config{
		RootCAs:      pool,
		Certificates: []tls.Certificate{cert},
		ServerName:   cfg.serverName,
		MinVersion:   tls.VersionTLS12,
	}
	if cfg.insecure {
		tc.InsecureSkipVerify = true
	}
	return tc, nil
}

func runPublish(cfg config, tlsCfg *tls.Config) error {
	conn, err := amqp.DialTLS(amqpURL(cfg), tlsCfg)
	if err != nil {
		return err
	}
	defer conn.Close()
	ch, err := conn.Channel()
	if err != nil {
		return err
	}
	defer ch.Close()
	return ch.Publish(cfg.exchange, cfg.publishKey, false, false, amqp.Publishing{
		ContentType: "text/plain", DeliveryMode: amqp.Transient, Body: []byte(cfg.publishBody),
	})
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func envIntOr(k string, def int) int {
	if v := os.Getenv(k); v != "" {
		var n int
		if _, err := fmt.Sscanf(v, "%d", &n); err == nil {
			return n
		}
	}
	return def
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n] + "..."
}
