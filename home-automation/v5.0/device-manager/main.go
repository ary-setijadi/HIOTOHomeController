// Command device-manager runs on the Orange Pi. It is the device-provisioning
// service: it registers devices, issues each a client certificate (CN = serial)
// signed by the internal CA, and serves a QR code that a device can scan to
// enroll itself. Certificates are stored alongside the existing ones in
// /etc/rabbitmq/certs/.
package main

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/json"
	"encoding/pem"
	"flag"
	"fmt"
	"log"
	"math/big"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

type config struct {
	caFile      string
	caKeyFile   string
	certDir     string
	registry    string
	httpPort    int
	broker      string // e.g. mqtts://192.168.137.44:8883
	publicHost  string // e.g. 192.168.137.44 (for the enroll URL in the QR)
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
	flag.StringVar(&c.caFile, "ca", envOr("CA_FILE", "/etc/rabbitmq/certs/ca.crt"), "CA certificate")
	flag.StringVar(&c.caKeyFile, "ca-key", envOr("CA_KEY_FILE", "/etc/rabbitmq/certs/ca.key"), "CA private key")
	flag.StringVar(&c.certDir, "cert-dir", envOr("CERT_DIR", "/etc/rabbitmq/certs"), "certificate directory")
	flag.StringVar(&c.registry, "registry", envOr("REGISTRY_FILE", "/var/lib/homeautomation/devices.json"), "device registry file")
	flag.IntVar(&c.httpPort, "http-port", envIntOr("HTTP_PORT", 8081), "HTTP API port")
	flag.StringVar(&c.broker, "broker", envOr("BROKER_URL", "mqtts://192.168.137.44:8883"), "broker URL for QR")
	flag.StringVar(&c.publicHost, "public-host", envOr("PUBLIC_HOST", "192.168.137.44"), "public host for enroll URL")
	flag.Parse()
	return c
}

type device struct {
	Serial    string `json:"serial"`
	Type      int    `json:"type"`
	Kind      string `json:"kind"`
	Token     string `json:"token"`
	CreatedAt string `json:"created_at"`
}

type registry struct {
	mu      sync.RWMutex
	file    string
	devices map[string]device
}

func loadRegistry(file string) *registry {
	r := &registry{file: file, devices: map[string]device{}}
	if b, err := os.ReadFile(file); err == nil {
		_ = json.Unmarshal(b, &r.devices)
	}
	return r
}
func (r *registry) save() {
	r.mu.RLock()
	b, _ := json.MarshalIndent(r.devices, "", "  ")
	r.mu.RUnlock()
	_ = os.MkdirAll(filepath.Dir(r.file), 0755)
	_ = os.WriteFile(r.file, b, 0644)
}
func (r *registry) list() []device {
	r.mu.RLock()
	defer r.mu.RUnlock()
	out := make([]device, 0, len(r.devices))
	for _, d := range r.devices {
		out = append(out, d)
	}
	return out
}
func (r *registry) get(serial string) (device, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	d, ok := r.devices[serial]
	return d, ok
}
func (r *registry) put(d device) {
	r.mu.Lock()
	r.devices[d.Serial] = d
	r.mu.Unlock()
	r.save()
}
func (r *registry) remove(serial string) {
	r.mu.Lock()
	delete(r.devices, serial)
	r.mu.Unlock()
	r.save()
}

func newToken() string {
	b := make([]byte, 16)
	_, _ = rand.Read(b)
	return fmt.Sprintf("%x", b)
}

// generateDeviceCert signs a client certificate (CN = serial) with the CA.
func generateDeviceCert(serial string, cfg config) ([]byte, []byte, error) {
	caPEM, err := os.ReadFile(cfg.caFile)
	if err != nil {
		return nil, nil, err
	}
	caKeyPEM, err := os.ReadFile(cfg.caKeyFile)
	if err != nil {
		return nil, nil, err
	}
	caPair, err := tls.X509KeyPair(caPEM, caKeyPEM)
	if err != nil {
		return nil, nil, err
	}
	caX509, err := x509.ParseCertificate(caPair.Certificate[0])
	if err != nil {
		return nil, nil, err
	}

	devKey, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, nil, err
	}
	serialNum, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return nil, nil, err
	}
	tmpl := &x509.Certificate{
		SerialNumber: serialNum,
		Subject:      pkix.Name{CommonName: serial},
		NotBefore:    time.Now().Add(-1 * time.Hour),
		NotAfter:     time.Now().Add(365 * 24 * time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature | x509.KeyUsageKeyEncipherment,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth},
	}
	der, err := x509.CreateCertificate(rand.Reader, tmpl, caX509, &devKey.PublicKey, caPair.PrivateKey)
	if err != nil {
		return nil, nil, err
	}
	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "RSA PRIVATE KEY", Bytes: x509.MarshalPKCS1PrivateKey(devKey)})
	return certPEM, keyPEM, nil
}

func writeDeviceCert(serial string, certPEM, keyPEM []byte, cfg config) error {
	certPath := filepath.Join(cfg.certDir, serial+".crt")
	keyPath := filepath.Join(cfg.certDir, serial+".key")
	if err := os.WriteFile(certPath, certPEM, 0644); err != nil {
		return err
	}
	if err := os.WriteFile(keyPath, keyPEM, 0600); err != nil {
		return err
	}
	_ = exec.Command("chown", "rabbitmq:rabbitmq", certPath, keyPath).Run()
	return nil
}

func removeDeviceCert(serial string, cfg config) {
	_ = os.Remove(filepath.Join(cfg.certDir, serial+".crt"))
	_ = os.Remove(filepath.Join(cfg.certDir, serial+".key"))
}

// enrollJSON is the payload encoded into the QR code.
func enrollJSON(d device, cfg config) string {
	u := fmt.Sprintf("http://%s:%d/api/enroll/%s?token=%s", cfg.publicHost, cfg.httpPort, d.Serial, d.Token)
	b, _ := json.Marshal(map[string]interface{}{
		"v":      1,
		"serial": d.Serial,
		"broker": cfg.broker,
		"enroll": u,
	})
	return string(b)
}

func qrPNG(data string) ([]byte, error) {
	cmd := exec.Command("qrencode", "-o", "-", "-t", "PNG", "-s", "6", "-m", "2", data)
	return cmd.Output()
}

func main() {
	cfg := parseConfig()
	reg := loadRegistry(cfg.registry)

	mux := http.NewServeMux()

	// register a new device: issue cert + return enroll info
	mux.HandleFunc("/api/register", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(rw, "method not allowed", 405)
			return
		}
		var b struct {
			Serial string `json:"serial"`
			Type   int    `json:"type"`
			Kind   string `json:"kind"`
		}
		if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
			http.Error(rw, "bad json", 400)
			return
		}
		b.Serial = strings.TrimSpace(b.Serial)
		if b.Serial == "" {
			http.Error(rw, "serial required", 400)
			return
		}
		if _, exists := reg.get(b.Serial); exists {
			http.Error(rw, "serial already registered", 409)
			return
		}
		if b.Type != 2 {
			b.Type = 1
		}
		if b.Kind != "actuator" {
			b.Kind = "sensor"
		}
		certPEM, keyPEM, err := generateDeviceCert(b.Serial, cfg)
		if err != nil {
			http.Error(rw, "cert generation: "+err.Error(), 500)
			return
		}
		if err := writeDeviceCert(b.Serial, certPEM, keyPEM, cfg); err != nil {
			http.Error(rw, "write cert: "+err.Error(), 500)
			return
		}
		d := device{Serial: b.Serial, Type: b.Type, Kind: b.Kind, Token: newToken(), CreatedAt: time.Now().UTC().Format(time.RFC3339)}
		reg.put(d)
		log.Printf("registered device %s (%s type %d)", d.Serial, d.Kind, d.Type)
		rw.Header().Set("Content-Type", "application/json")
		json.NewEncoder(rw).Encode(map[string]interface{}{
			"serial": d.Serial, "type": d.Type, "kind": d.Kind,
			"broker": cfg.broker, "token": d.Token,
			"enroll": fmt.Sprintf("http://%s:%d/api/enroll/%s?token=%s", cfg.publicHost, cfg.httpPort, d.Serial, d.Token),
			"qr":     fmt.Sprintf("http://%s:%d/api/devices/%s/qr.png", cfg.publicHost, cfg.httpPort, d.Serial),
		})
	})

	// list devices
	mux.HandleFunc("/api/devices", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method == http.MethodGet {
			rw.Header().Set("Content-Type", "application/json")
			json.NewEncoder(rw).Encode(reg.list())
			return
		}
		http.Error(rw, "method not allowed", 405)
	})

	// QR code for a device
	mux.HandleFunc("/api/devices/", func(rw http.ResponseWriter, r *http.Request) {
		parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/devices/"), "/")
		if len(parts) != 2 || parts[1] != "qr.png" {
			http.Error(rw, "not found", 404)
			return
		}
		serial := parts[0]
		d, ok := reg.get(serial)
		if !ok {
			http.Error(rw, "unknown device", 404)
			return
		}
		if r.Method == http.MethodGet {
			png, err := qrPNG(enrollJSON(d, cfg))
			if err != nil {
				http.Error(rw, "qr: "+err.Error(), 500)
				return
			}
			rw.Header().Set("Content-Type", "image/png")
			rw.Write(png)
			return
		}
		if r.Method == http.MethodDelete {
			removeDeviceCert(serial, cfg)
			reg.remove(serial)
			log.Printf("revoked device %s", serial)
			rw.Header().Set("Content-Type", "application/json")
			json.NewEncoder(rw).Encode(map[string]bool{"ok": true})
			return
		}
		http.Error(rw, "method not allowed", 405)
	})

	// enrollment: return the device's ca + cert + key (validated by token)
	mux.HandleFunc("/api/enroll/", func(rw http.ResponseWriter, r *http.Request) {
		serial := strings.TrimPrefix(r.URL.Path, "/api/enroll/")
		serial = strings.TrimSuffix(serial, "/")
		if serial == "" {
			http.Error(rw, "serial required", 400)
			return
		}
		d, ok := reg.get(serial)
		if !ok {
			http.Error(rw, "unknown device", 404)
			return
		}
		if r.URL.Query().Get("token") != d.Token {
			http.Error(rw, "invalid token", 403)
			return
		}
		certPEM, err := os.ReadFile(filepath.Join(cfg.certDir, serial+".crt"))
		if err != nil {
			http.Error(rw, "cert not found", 404)
			return
		}
		keyPEM, err := os.ReadFile(filepath.Join(cfg.certDir, serial+".key"))
		if err != nil {
			http.Error(rw, "key not found", 404)
			return
		}
		caPEM, err := os.ReadFile(cfg.caFile)
		if err != nil {
			http.Error(rw, "ca not found", 404)
			return
		}
		rw.Header().Set("Content-Type", "application/json")
		json.NewEncoder(rw).Encode(map[string]interface{}{
			"serial": serial,
			"broker": cfg.broker,
			"ca":     string(caPEM),
			"cert":   string(certPEM),
			"key":    string(keyPEM),
		})
	})

	addr := fmt.Sprintf(":%d", cfg.httpPort)
	log.Printf("device-manager API on %s (registry=%s, cert-dir=%s)", addr, cfg.registry, cfg.certDir)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Fatalf("http: %v", err)
	}
}
