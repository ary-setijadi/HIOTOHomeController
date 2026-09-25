package main

import (
	"bufio"
	"encoding/json"
	"fmt"
	"log"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

// Bounded telemetry store. The dashboard's live history is held in memory
// (capped at memLimitBytes) so it never overwhelms the Orange Pi's RAM, and
// overflow is appended to rollover files of fileLimitBytes each. At most
// maxTelFiles files are kept on disk; the oldest is deleted first.

const (
	memLimitBytes  = 10 << 20 // 10 MB in-memory
	fileLimitBytes = 10 << 20 // 10 MB per rollover file
	maxTelFiles    = 5        // 50 MB on-disk total
)

type telemetryPoint struct {
	TS     string  `json:"ts"`
	GUID   string  `json:"guid"`
	Name   string  `json:"name"`
	Metric string  `json:"metric"`
	Value  float64 `json:"value"`
}

type telemetryStore struct {
	mu      sync.Mutex
	mem     []telemetryPoint
	memSize int64
	dir     string
	cur     *os.File
	curSize int64
	seq     int
}

// tel is a package-global store; recordTelemetry no-ops until initTelemetry runs.
var tel = &telemetryStore{}

func initTelemetry(dir string) {
	t := &telemetryStore{dir: dir}
	_ = os.MkdirAll(dir, 0755)
	files, _ := filepath.Glob(filepath.Join(dir, "telemetry-*.jsonl"))
	maxSeq := 0
	for _, f := range files {
		if n := telSeq(f); n > maxSeq {
			maxSeq = n
		}
	}
	t.seq = maxSeq + 1
	// Reuse the newest existing file if it still has room.
	if maxSeq > 0 {
		path := filepath.Join(dir, fmt.Sprintf("telemetry-%d.jsonl", maxSeq))
		if f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644); err == nil {
			if st, err := f.Stat(); err == nil && st.Size() < fileLimitBytes {
				t.cur = f
				t.curSize = st.Size()
			} else {
				_ = f.Close()
			}
		}
	}
	tel = t
	go t.flushLoop()
	log.Printf("telemetry store: dir=%s (mem ≤10MB, %d files ≤10MB each)", dir, maxTelFiles)
}

func telSeq(path string) int {
	base := filepath.Base(path)
	n, _ := strconv.Atoi(strings.TrimSuffix(strings.TrimPrefix(base, "telemetry-"), ".jsonl"))
	return n
}

func (t *telemetryStore) flushLoop() {
	tk := time.NewTicker(5 * time.Minute)
	defer tk.Stop()
	for range tk.C {
		t.flush()
	}
}

// recordTelemetry appends one reading to the bounded store (throttled per
// guid+metric so a fast stream can't churn the buffer/flush path).
func recordTelemetry(guid, name, metric string, value float64) {
	if tel == nil || tel.dir == "" {
		return
	}
	if !throttled("tel:" + guid + "|" + metric) {
		return
	}
	tel.append(guid, name, metric, value)
}

func (t *telemetryStore) append(guid, name, metric string, value float64) {
	t.mu.Lock()
	defer t.mu.Unlock()
	p := telemetryPoint{TS: time.Now().Format(time.RFC3339Nano), GUID: guid, Name: name, Metric: metric, Value: value}
	b, _ := json.Marshal(p)
	t.mem = append(t.mem, p)
	t.memSize += int64(len(b)) + 1
	if t.memSize >= memLimitBytes {
		t.flushLocked()
	}
}

func (t *telemetryStore) flush() {
	t.mu.Lock()
	defer t.mu.Unlock()
	t.flushLocked()
}

func (t *telemetryStore) flushLocked() {
	if len(t.mem) == 0 || t.dir == "" {
		return
	}
	if t.cur == nil {
		f, err := os.OpenFile(filepath.Join(t.dir, fmt.Sprintf("telemetry-%d.jsonl", t.seq)), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
		if err != nil {
			t.mem = t.mem[:0]
			t.memSize = 0
			return
		}
		t.cur = f
		t.curSize = 0
	}
	w := bufio.NewWriter(t.cur)
	for _, p := range t.mem {
		b, _ := json.Marshal(p)
		b = append(b, '\n')
		if t.curSize+int64(len(b)) > fileLimitBytes {
			_ = w.Flush()
			_ = t.cur.Close()
			t.seq++
			t.cur, _ = os.OpenFile(filepath.Join(t.dir, fmt.Sprintf("telemetry-%d.jsonl", t.seq)), os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
			t.curSize = 0
			w = bufio.NewWriter(t.cur)
		}
		_, _ = w.Write(b)
		t.curSize += int64(len(b))
	}
	_ = w.Flush()
	t.mem = t.mem[:0]
	t.memSize = 0
	t.enforceFileCap()
}

func (t *telemetryStore) enforceFileCap() {
	files, _ := filepath.Glob(filepath.Join(t.dir, "telemetry-*.jsonl"))
	if len(files) <= maxTelFiles {
		return
	}
	sort.Slice(files, func(i, j int) bool { return telSeq(files[i]) < telSeq(files[j]) })
	for _, f := range files[:len(files)-maxTelFiles] {
		_ = os.Remove(f)
	}
}

// recent returns the newest up-to-`limit` points matching guid/metric ("" = any)
// from the in-memory buffer only.
func (t *telemetryStore) recent(guid, metric string, limit int) []telemetryPoint {
	t.mu.Lock()
	defer t.mu.Unlock()
	out := make([]telemetryPoint, 0, limit)
	for i := len(t.mem) - 1; i >= 0 && len(out) < limit; i-- {
		p := t.mem[i]
		if guid != "" && p.GUID != guid {
			continue
		}
		if metric != "" && p.Metric != metric {
			continue
		}
		out = append(out, p)
	}
	return out
}

// history returns the newest up-to-`limit` points matching guid/metric ("" = any),
// merging the in-memory buffer with the on-disk rollover files. The in-memory
// buffer is newer than the files; files are scanned newest-sequence-first and,
// within a file, newest-line-first. Results are newest-first.
func (t *telemetryStore) history(guid, metric string, limit int) []telemetryPoint {
	t.mu.Lock()
	defer t.mu.Unlock()

	match := func(p telemetryPoint) bool {
		return (guid == "" || p.GUID == guid) && (metric == "" || p.Metric == metric)
	}

	out := make([]telemetryPoint, 0, limit)

	// 1. newest in-memory points first (t.mem is chronological ascending).
	for i := len(t.mem) - 1; i >= 0 && len(out) < limit; i-- {
		if match(t.mem[i]) {
			out = append(out, t.mem[i])
		}
	}
	if len(out) >= limit {
		return out
	}

	// 2. on-disk rollover files, newest sequence first.
	files, _ := filepath.Glob(filepath.Join(t.dir, "telemetry-*.jsonl"))
	sort.Slice(files, func(i, j int) bool { return telSeq(files[i]) > telSeq(files[j]) })
	for _, f := range files {
		if len(out) >= limit {
			break
		}
		data, err := os.ReadFile(f)
		if err != nil {
			continue
		}
		lines := strings.Split(string(data), "\n")
		for i := len(lines) - 1; i >= 0 && len(out) < limit; i-- {
			ln := strings.TrimSpace(lines[i])
			if ln == "" {
				continue
			}
			var p telemetryPoint
			if json.Unmarshal([]byte(ln), &p) != nil {
				continue
			}
			if match(p) {
				out = append(out, p)
			}
		}
	}
	return out
}

func (t *telemetryStore) stats() (memBytes, diskBytes int64, files int) {
	t.mu.Lock()
	defer t.mu.Unlock()
	memBytes = t.memSize
	fl, _ := filepath.Glob(filepath.Join(t.dir, "telemetry-*.jsonl"))
	for _, f := range fl {
		if st, err := os.Stat(f); err == nil {
			diskBytes += st.Size()
		}
	}
	return memBytes, diskBytes, len(fl)
}
