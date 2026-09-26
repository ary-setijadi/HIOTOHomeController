package main

import (
	"encoding/base64"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
	"unicode/utf8"
)

// mirrorEntry is one consumed message, stored as a JSON line.
type mirrorEntry struct {
	TS         string `json:"ts"`
	RoutingKey string `json:"routing_key"`
	Body       string `json:"body"`
	BodyB64    string `json:"body_b64,omitempty"` // base64 of raw body, only when non-UTF8
}

// store writes consumed messages to daily JSONL files (messages-YYYY-MM-DD.jsonl)
// and optionally prunes files older than a retention window.
type store struct {
	mu        sync.Mutex
	dir       string
	retention int
	cur       *os.File
	curDay    string
}

func newStore(dir string, retention int) (*store, error) {
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, err
	}
	s := &store{dir: dir, retention: retention}
	if retention > 0 {
		s.prune()
	}
	return s, nil
}

func (s *store) pathFor(day string) string {
	return filepath.Join(s.dir, "messages-"+day+".jsonl")
}

func (s *store) append(routingKey string, body []byte) {
	s.mu.Lock()
	defer s.mu.Unlock()

	day := time.Now().UTC().Format("2006-01-02")
	if s.curDay != day {
		if s.cur != nil {
			_ = s.cur.Close()
		}
		f, err := os.OpenFile(s.pathFor(day), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
		if err != nil {
			return
		}
		s.cur, s.curDay = f, day
	}

	e := mirrorEntry{
		TS:         time.Now().UTC().Format(time.RFC3339Nano),
		RoutingKey: routingKey,
		Body:       string(body),
	}
	if !utf8.Valid(body) {
		e.BodyB64 = base64.StdEncoding.EncodeToString(body)
	}
	b, _ := json.Marshal(e)
	b = append(b, '\n')
	_, _ = s.cur.Write(b)
}

func (s *store) close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.cur != nil {
		_ = s.cur.Close()
		s.cur = nil
	}
}

func (s *store) prune() {
	files, _ := filepath.Glob(filepath.Join(s.dir, "messages-*.jsonl"))
	sort.Strings(files)
	cutoff := time.Now().UTC().AddDate(0, 0, -s.retention).Format("2006-01-02")
	for _, f := range files {
		base := filepath.Base(f)
		day := strings.TrimSuffix(strings.TrimPrefix(base, "messages-"), ".jsonl")
		if day != "" && day < cutoff {
			_ = os.Remove(f)
		}
	}
}
