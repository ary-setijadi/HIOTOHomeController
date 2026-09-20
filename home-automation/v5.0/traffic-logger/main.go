// Command traffic-logger runs on the Orange Pi. It subscribes to home.# over
// AMQPS and appends every message to a rolling 100 MB file. It also serves an
// HTTP API so the browser monitor can page through history without loading
// more than a small (<=10%) portion into memory at a time.
package main

import (
	"bufio"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"strconv"
	"sync"
	"time"

	amqp "github.com/rabbitmq/amqp091-go"
)

const (
	maxFileBytes = 100 * 1024 * 1024 // 100 MB hard limit
	keepBytes    = 80 * 1024 * 1024  // on rotate keep the newest 80 MB
	maxTailBytes = 10 * 1024 * 1024  // <=10% of the file loaded at once
)

type config struct {
	host     string
	port     int
	vhost    string
	user     string
	password string
	exchange string
	caFile   string
	certFile string
	keyFile  string
	logFile  string
	httpPort int
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
	flag.StringVar(&c.caFile, "ca", envOr("TLS_CA", "/etc/rabbitmq/certs/ca.crt"), "CA certificate")
	flag.StringVar(&c.certFile, "cert", envOr("TLS_CERT", "/etc/rabbitmq/certs/monitor.crt"), "client certificate")
	flag.StringVar(&c.keyFile, "key", envOr("TLS_KEY", "/etc/rabbitmq/certs/monitor.key"), "client key")
	flag.StringVar(&c.logFile, "log", envOr("LOG_FILE", "/var/lib/homeautomation/traffic.log"), "traffic log file")
	flag.IntVar(&c.httpPort, "http-port", envIntOr("HTTP_PORT", 8080), "HTTP query port")
	flag.Parse()
	return c
}

type logRecord struct {
	T   string          `json:"t"`   // receive time (RFC3339)
	RK  string          `json:"rk"`  // routing key (dots)
	Env json.RawMessage `json:"env"` // original envelope body
}

// appender wraps the log file + a bufio writer guarded by a mutex.
type appender struct {
	mu    sync.RWMutex
	path  string
	f     *os.File
	w     *bufio.Writer
	bytes int64
}

func openAppender(path string) (*appender, error) {
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return nil, err
	}
	var sz int64
	if st, err := f.Stat(); err == nil {
		sz = st.Size()
	}
	return &appender{path: path, f: f, w: bufio.NewWriterSize(f, 64*1024), bytes: sz}, nil
}

func (a *appender) write(line []byte) {
	a.mu.Lock()
	defer a.mu.Unlock()
	n, _ := a.w.Write(line)
	a.bytes += int64(n)
	_ = a.w.Flush()
	if a.bytes > maxFileBytes {
		a.rotateLocked()
	}
}

// rotateLocked keeps only the newest keepBytes of the file (streaming, so it
// never loads more than a small buffer into memory). Caller holds a.mu.
func (a *appender) rotateLocked() {
	_ = a.w.Flush()
	_ = a.f.Close()

	in, err := os.Open(a.path)
	if err != nil {
		log.Printf("rotate open: %v", err)
		return
	}
	st, err := in.Stat()
	if err != nil || st.Size() <= keepBytes {
		in.Close()
		a.reopen()
		return
	}
	if _, err := in.Seek(st.Size()-keepBytes, io.SeekStart); err != nil {
		in.Close()
		a.reopen()
		return
	}
	tmp := a.path + ".tmp"
	out, err := os.Create(tmp)
	if err != nil {
		in.Close()
		a.reopen()
		return
	}
	n, err := io.Copy(out, in)
	out.Close()
	in.Close()
	if err != nil {
		os.Remove(tmp)
		a.reopen()
		return
	}
	if err := os.Rename(tmp, a.path); err != nil {
		os.Remove(tmp)
	}
	a.bytes = n
	log.Printf("rotated %s -> kept %d bytes", a.path, n)
	a.reopen()
}

func (a *appender) reopen() {
	f, err := os.OpenFile(a.path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		log.Printf("reopen: %v", err)
		a.f = nil
		a.w = nil
		return
	}
	var sz int64
	if st, err := f.Stat(); err == nil {
		sz = st.Size()
	}
	a.f = f
	a.w = bufio.NewWriterSize(f, 64*1024)
	a.bytes = sz
}

func (a *appender) close() {
	a.mu.Lock()
	defer a.mu.Unlock()
	_ = a.w.Flush()
	_ = a.f.Close()
}

// tail streams the newest `n` bytes of the file to w (skipping a leading
// partial line so the client always starts on a whole JSON record).
func (a *appender) tail(w io.Writer, n int64) {
	a.mu.RLock()
	defer a.mu.RUnlock()
	if a.f == nil {
		return
	}
	f, err := os.Open(a.path)
	if err != nil {
		return
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return
	}
	offset := int64(0)
	if st.Size() > n {
		offset = st.Size() - n
	}
	if _, err := f.Seek(offset, io.SeekStart); err != nil {
		return
	}
	br := bufio.NewReader(f)
	if offset > 0 {
		br.ReadString('\n') // discard partial first line
	}
	io.Copy(w, br)
}

func (a *appender) size() int64 {
	a.mu.RLock()
	defer a.mu.RUnlock()
	return a.bytes
}

func main() {
	cfg := parseConfig()

	ca, err := os.ReadFile(cfg.caFile)
	if err != nil {
		log.Fatalf("read CA: %v", err)
	}
	pool := x509.NewCertPool()
	if !pool.AppendCertsFromPEM(ca) {
		log.Fatalf("append CA: no certificates")
	}
	cert, err := tls.LoadX509KeyPair(cfg.certFile, cfg.keyFile)
	if err != nil {
		log.Fatalf("load keypair: %v", err)
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
	q, err := ch.QueueDeclare("", false, true, true, false, nil)
	if err != nil {
		log.Fatalf("queue: %v", err)
	}
	if err := ch.QueueBind(q.Name, "home.#", cfg.exchange, false, nil); err != nil {
		log.Fatalf("bind: %v", err)
	}
	msgs, err := ch.Consume(q.Name, "traffic-logger", true, false, false, false, nil)
	if err != nil {
		log.Fatalf("consume: %v", err)
	}

	if err := os.MkdirAll(filepathDir(cfg.logFile), 0755); err != nil {
		log.Fatalf("mkdir: %v", err)
	}
	ap, err := openAppender(cfg.logFile)
	if err != nil {
		log.Fatalf("open log: %v", err)
	}
	defer ap.close()

	// HTTP query API (plaintext, internal ICS network only).
	mux := http.NewServeMux()
	mux.HandleFunc("/tail", func(rw http.ResponseWriter, r *http.Request) {
		n := int64(1024 * 1024)
		if v := r.URL.Query().Get("bytes"); v != "" {
			if p, err := strconv.ParseInt(v, 10, 64); err == nil && p > 0 {
				n = p
			}
		}
		if n > maxTailBytes {
			n = maxTailBytes
		}
		rw.Header().Set("Content-Type", "application/x-ndjson")
		ap.tail(rw, n)
	})
	mux.HandleFunc("/stats", func(rw http.ResponseWriter, r *http.Request) {
		rw.Header().Set("Content-Type", "application/json")
		json.NewEncoder(rw).Encode(map[string]interface{}{
			"fileBytes": ap.size(),
			"maxBytes":  maxFileBytes,
			"tailMax":   maxTailBytes,
		})
	})
	go func() {
		log.Printf("traffic-logger HTTP API on :%d (file=%s, max=%dMB)", cfg.httpPort, cfg.logFile, maxFileBytes/1024/1024)
		if err := http.ListenAndServe(fmt.Sprintf(":%d", cfg.httpPort), mux); err != nil {
			log.Fatalf("http: %v", err)
		}
	}()

	log.Printf("traffic-logger started: consuming %s.# -> %s", cfg.exchange, cfg.logFile)
	for d := range msgs {
		rec := logRecord{T: time.Now().UTC().Format(time.RFC3339Nano), RK: d.RoutingKey, Env: json.RawMessage(d.Body)}
		line, err := json.Marshal(rec)
		if err != nil {
			continue
		}
		line = append(line, '\n')
		ap.write(line)
	}
}

func filepathDir(p string) string {
	for i := len(p) - 1; i >= 0; i-- {
		if p[i] == '/' {
			return p[:i]
		}
	}
	return "."
}
