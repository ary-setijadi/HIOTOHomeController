package main

import (
	"embed"
	"encoding/json"
	"fmt"
	"io/fs"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"sync"
)

//go:embed web
var webFS embed.FS

// liveEntry is a device's last-known value for the live dashboard.
type liveEntry struct {
	Kind     string  `json:"kind"`
	Type     int     `json:"type"` // 1 digital, 2 analog
	Value    float64 `json:"value"`
	Name     string  `json:"name"`
	Category string  `json:"category"`
}

var (
	liveMu       sync.RWMutex
	liveState    = map[string]liveEntry{}
	liveClients  = map[chan []byte]struct{}{}
	liveClientMu sync.Mutex
)

// recordLive stores a device value and broadcasts it to SSE clients.
func recordLive(guid, kind string, typ int, value float64, name, category string) {
	if guid == "" {
		return
	}
	floor := floorLabel(name)
	liveMu.Lock()
	liveState[guid] = liveEntry{Kind: kind, Type: typ, Value: value, Name: name, Category: category}
	liveMu.Unlock()

	msg, _ := json.Marshal(map[string]interface{}{
		"type": "state", "serial": guid, "kind": kind, "dtype": typ,
		"value": value, "name": name, "category": category, "floor": floor,
	})
	liveClientMu.Lock()
	for ch := range liveClients {
		select {
		case ch <- msg:
		default:
		}
	}
	liveClientMu.Unlock()

	// also push to WebSocket clients (the Android app's /api/ws)
	if wsMsg, err := json.Marshal(map[string]interface{}{
		"type": "device", "guid": guid, "value": value, "name": name, "category": category, "kind": kind, "floor": floor,
	}); err == nil {
		broadcastWS(wsMsg)
	}
}

// liveSnapshot returns ALL registered devices (not just those with a live
// value), so the dashboard shows the full fleet. Live values win; devices that
// haven't reported yet default to 0.
func liveSnapshot(db *DB) (sensors, actuators []map[string]interface{}) {
	devs, err := db.listDevices()
	if err != nil {
		return nil, nil
	}
	liveMu.RLock()
	defer liveMu.RUnlock()
	for _, d := range devs {
		value := 0.0
		if e, ok := liveState[d.GUID]; ok {
			value = e.Value
		}
		item := map[string]interface{}{
			"serial": d.GUID, "name": d.Name, "category": d.Type,
			"type": numericType(d.Type), "value": value, "floor": floorLabel(d.Name),
		}
		if d.Kind == KindSensor {
			sensors = append(sensors, item)
		} else {
			actuators = append(actuators, item)
		}
	}
	sort.Slice(sensors, func(i, j int) bool { return sensors[i]["name"].(string) < sensors[j]["name"].(string) })
	sort.Slice(actuators, func(i, j int) bool { return actuators[i]["name"].(string) < actuators[j]["name"].(string) })
	return sensors, actuators
}

// registerWebHandlers adds the live dashboard + control endpoints and the
// embedded manager/dashboard static files to an existing mux.
func registerWebHandlers(mux *http.ServeMux, db *DB, publish func(string, float64)) {
	// live state snapshot + SSE stream
	mux.HandleFunc("/api/state", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(rw, "method not allowed", 405)
			return
		}
		sensors, actuators := liveSnapshot(db)
		_ = json.NewEncoder(rw).Encode(map[string]interface{}{
			"connected": true, "sensors": sensors, "actuators": actuators,
		})
	})

	// Bounded telemetry history (in-memory, ≤10MB) + store stats.
	mux.HandleFunc("/api/telemetry", func(rw http.ResponseWriter, r *http.Request) {
		guid := r.URL.Query().Get("guid")
		metric := r.URL.Query().Get("metric")
		limit := 500
		if v := r.URL.Query().Get("limit"); v != "" {
			if n, err := strconv.Atoi(v); err == nil && n > 0 && n <= 5000 {
				limit = n
			}
		}
		pts := tel.history(guid, metric, limit)
		memBytes, diskBytes, files := tel.stats()
		_ = json.NewEncoder(rw).Encode(map[string]interface{}{
			"points": pts, "mem_bytes": memBytes, "disk_bytes": diskBytes, "files": files,
		})
	})

	mux.HandleFunc("/events", func(rw http.ResponseWriter, r *http.Request) {
		fl, ok := rw.(http.Flusher)
		if !ok {
			http.Error(rw, "streaming unsupported", 500)
			return
		}
		rw.Header().Set("Content-Type", "text/event-stream")
		rw.Header().Set("Cache-Control", "no-cache")
		rw.Header().Set("Connection", "keep-alive")
		ch := make(chan []byte, 64)
		liveClientMu.Lock()
		liveClients[ch] = struct{}{}
		liveClientMu.Unlock()
		defer func() {
			liveClientMu.Lock()
			delete(liveClients, ch)
			liveClientMu.Unlock()
		}()
		sensors, actuators := liveSnapshot(db)
		hello, _ := json.Marshal(map[string]interface{}{"type": "hello", "connected": true, "sensors": sensors, "actuators": actuators})
		_, _ = fmt.Fprintf(rw, "data: %s\n\n", hello)
		fl.Flush()
		ctx := r.Context()
		for {
			select {
			case <-ctx.Done():
				return
			case m := <-ch:
				_, _ = fmt.Fprintf(rw, "data: %s\n\n", m)
				fl.Flush()
			}
		}
	})

	// control endpoints (actuator control via the rule-engine publish path)
	readControlBody := func(r *http.Request) (string, float64, bool) {
		var b struct {
			Serial string  `json:"serial"`
			Value  float64 `json:"value"`
		}
		if json.NewDecoder(r.Body).Decode(&b) != nil {
			return "", 0, false
		}
		return strings.TrimSpace(b.Serial), b.Value, true
	}

	mux.HandleFunc("/api/override", func(rw http.ResponseWriter, r *http.Request) {
		serial, val, ok := readControlBody(r)
		if !ok || serial == "" {
			http.Error(rw, "serial required", 400)
			return
		}
		publish(serial, val)
		_ = json.NewEncoder(rw).Encode(map[string]bool{"ok": true})
	})
	mux.HandleFunc("/api/release", func(rw http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(rw).Encode(map[string]bool{"ok": true})
	})
	mux.HandleFunc("/api/sensor", func(rw http.ResponseWriter, r *http.Request) {
		serial, val, ok := readControlBody(r)
		if !ok || serial == "" {
			http.Error(rw, "serial required", 400)
			return
		}
		// manual sensor set: record locally and push a state value.
		recordLive(serial, "sensor", 1, val, "", "")
		publish(serial, val)
		_ = json.NewEncoder(rw).Encode(map[string]bool{"ok": true})
	})
	mux.HandleFunc("/api/sensor-release", func(rw http.ResponseWriter, r *http.Request) {
		_ = json.NewEncoder(rw).Encode(map[string]bool{"ok": true})
	})

	// aliases used by the dashboard's Devices tab (manager exposes them natively)
	mux.HandleFunc("/api/register-device", func(rw http.ResponseWriter, r *http.Request) {
		var dev Device
		if json.NewDecoder(r.Body).Decode(&dev) != nil {
			http.Error(rw, "bad json", 400)
			return
		}
		dev.GUID = strings.TrimSpace(dev.GUID)
		if dev.GUID == "" {
			http.Error(rw, "guid required", 400)
			return
		}
		if dev.Type == "" {
			dev.Type = "SENSOR"
		}
		_ = db.upsertDevice(dev)
		_ = json.NewEncoder(rw).Encode(map[string]interface{}{"guid": dev.GUID, "type": dev.Type, "device": dev})
	})
	mux.HandleFunc("/api/revoke-device/", func(rw http.ResponseWriter, r *http.Request) {
		guid := strings.TrimPrefix(r.URL.Path, "/api/revoke-device/")
		if guid == "" {
			http.Error(rw, "guid required", 400)
			return
		}
		_ = db.deleteDevice(guid)
		_ = json.NewEncoder(rw).Encode(map[string]bool{"ok": true})
	})

	// embedded static UI: manager at "/", dashboard at "/dashboard/"
	managerFS, _ := fs.Sub(webFS, "web/manager")
	dashboardFS, _ := fs.Sub(webFS, "web/dashboard")
	mux.Handle("/dashboard/", http.StripPrefix("/dashboard/", http.FileServer(http.FS(dashboardFS))))
	mux.Handle("/", http.FileServer(http.FS(managerFS)))
}
