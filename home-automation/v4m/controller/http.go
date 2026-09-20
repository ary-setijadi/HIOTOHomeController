package main

import (
	"encoding/csv"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"os/exec"
	"strconv"
	"strings"
)

// deviceCSVHeaders is the canonical column order for the CSV import/export
// (matches the Device JSON field names, i.e. the HIOTO registration scheme).
var deviceCSVHeaders = []string{
	"guid", "mac", "type", "name", "status", "status_device", "version",
	"minor", "category", "quantity", "room_id", "floor_id", "x_position", "y_position", "last_seen",
}

// startHTTP serves the device-management + rule-import API (plaintext, on the
// local ICS network). It owns the SQLite DB via the `db` handle and reloads the
// rule engine (`st`) when rules are imported.
func startHTTP(db *DB, st *state, cfg config, publish func(string, float64), deviceMap map[string]Device) {
	mux := http.NewServeMux()

	send := func(rw http.ResponseWriter, code int, obj interface{}) {
		rw.Header().Set("Content-Type", "application/json")
		rw.WriteHeader(code)
		_ = json.NewEncoder(rw).Encode(obj)
	}

	// QR payload: full HIOTO registration info + broker credentials, so a
	// dedicated registration app can onboard the device from a single scan.
	qrPayload := func(dev Device) string {
		b, _ := json.Marshal(map[string]interface{}{
			"v":          1,
			"guid":       dev.GUID,
			"type":       dev.Type,
			"name":       dev.Name,
			"mac":        dev.MAC,
			"version":    dev.Version,
			"minor":      dev.Minor,
			"quantity":   dev.Quantity,
			"room_id":    dev.RoomID,
			"floor_id":   dev.FloorID,
			"x_position": dev.XPosition,
			"y_position": dev.YPosition,
			"broker":     fmt.Sprintf("mqtt://%s:%d", cfg.brokerHost, cfg.mqttPort),
			"user":       cfg.user,
			"pass":       cfg.password,
		})
		return string(b)
	}

	// POST /api/register  — full HIOTO registration {guid, mac, type, name,
	// version, minor, quantity, room_id, floor_id, x_position, y_position, ...}
	mux.HandleFunc("/api/register", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		var dev Device
		if err := json.NewDecoder(r.Body).Decode(&dev); err != nil {
			send(rw, 400, map[string]string{"error": "bad json"})
			return
		}
		dev.GUID = strings.TrimSpace(dev.GUID)
		if dev.GUID == "" {
			send(rw, 400, map[string]string{"error": "guid required"})
			return
		}
		if dev.Type == "" {
			dev.Type = "SENSOR"
		}
		if err := db.upsertDevice(dev); err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		log.Printf("[register] %s (%s)", dev.GUID, dev.Type)
		send(rw, 200, map[string]interface{}{
			"guid":       dev.GUID,
			"type":       dev.Type,
			"kind":       kindOf(dev.Type),
			"value_type": valueTypeOf(dev.Type),
			"broker":     fmt.Sprintf("mqtt://%s:%d", cfg.brokerHost, cfg.mqttPort),
			"qr":         fmt.Sprintf("http://%s:%d/api/devices/%s/qr.png", cfg.brokerHost, cfg.httpPort, dev.GUID),
			"device":     dev,
		})
	})

	// GET /api/devices
	mux.HandleFunc("/api/devices", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		devs, err := db.listDevices()
		if err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		send(rw, 200, devs)
	})

	// GET/DELETE /api/devices/<guid>  and  GET /api/devices/<guid>/qr.png
	mux.HandleFunc("/api/devices/", func(rw http.ResponseWriter, r *http.Request) {
		parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/api/devices/"), "/")
		if len(parts) == 0 || parts[0] == "" {
			send(rw, 404, map[string]string{"error": "not found"})
			return
		}
		guid := parts[0]

		if len(parts) == 2 && parts[1] == "qr.png" && r.Method == http.MethodGet {
			dev, ok := db.getDevice(guid)
			if !ok {
				send(rw, 404, map[string]string{"error": "device not found"})
				return
			}
			png, err := qrPNG(qrPayload(dev))
			if err != nil {
				send(rw, 500, map[string]string{"error": err.Error()})
				return
			}
			rw.Header().Set("Content-Type", "image/png")
			rw.Write(png)
			return
		}

		if len(parts) == 1 && r.Method == http.MethodGet {
			dev, ok := db.getDevice(guid)
			if !ok {
				send(rw, 404, map[string]string{"error": "device not found"})
				return
			}
			send(rw, 200, dev)
			return
		}

		if len(parts) == 1 && r.Method == http.MethodDelete {
			if err := db.deleteDevice(guid); err != nil {
				send(rw, 500, map[string]string{"error": err.Error()})
				return
			}
			log.Printf("[revoke] %s", guid)
			send(rw, 200, map[string]bool{"ok": true})
			return
		}

		if len(parts) == 1 && r.Method == http.MethodPut {
			var b struct {
				Name string `json:"name"`
				Type string `json:"type"`
				MAC  string `json:"mac"`
			}
			if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
				send(rw, 400, map[string]string{"error": "bad json"})
				return
			}
			// Partial update: only overwrite the fields that were provided.
			if cur, ok := db.getDevice(guid); ok {
				name, typ, mac := b.Name, b.Type, b.MAC
				if name == "" {
					name = cur.Name
				}
				if typ == "" {
					typ = cur.Type
				}
				if mac == "" {
					mac = cur.MAC
				}
				if err := db.updateDevice(guid, name, typ, mac); err != nil {
					send(rw, 500, map[string]string{"error": err.Error()})
					return
				}
				// Keep the in-memory registry in sync so live broadcasts
				// don't push the stale name back to the dashboard.
				if dev, ok := deviceMap[guid]; ok {
					dev.Name = name
					dev.Type = typ
					dev.MAC = mac
					deviceMap[guid] = dev
				}
			}
			log.Printf("[update] %s", guid)
			send(rw, 200, map[string]bool{"ok": true})
			return
		}
		send(rw, 404, map[string]string{"error": "not found"})
	})

	// POST /api/import-rules  {rules:[{input_guid,input_value,output_guid,output_value}]}
	mux.HandleFunc("/api/import-rules", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		var b struct {
			Rules []RuleDevice `json:"rules"`
		}
		if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
			send(rw, 400, map[string]string{"error": "bad json"})
			return
		}
		if err := db.importRuleDevices(b.Rules); err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		reloadRules(st, db)
		log.Printf("[import] %d rule_devices", len(b.Rules))
		send(rw, 200, map[string]int{"imported": len(b.Rules)})
	})

	// POST /api/import-devices  {devices:[{guid,mac,type,name,status,last_seen}]}
	mux.HandleFunc("/api/import-devices", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		var b struct {
			Devices []Device `json:"devices"`
		}
		if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
			send(rw, 400, map[string]string{"error": "bad json"})
			return
		}
		var devs []Device
		for _, d := range b.Devices {
			guid := strings.TrimLeft(strings.TrimSpace(d.GUID), ": \t")
			if guid == "" {
				continue
			}
			d.GUID = guid
			if strings.TrimSpace(d.Type) == "" {
				d.Type = "SENSOR"
			}
			devs = append(devs, d)
		}
		if err := db.importDevices(devs); err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		log.Printf("[import] %d devices", len(devs))
		send(rw, 200, map[string]int{"imported": len(devs)})
	})

	// POST /api/clean  — clear all devices + rules (fresh slate).
	mux.HandleFunc("/api/clean", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		if err := db.clearDevices(); err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		if err := db.clearRuleDevices(); err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		reloadRules(st, db)
		log.Printf("[clean] cleared all devices + rules")
		send(rw, 200, map[string]bool{"ok": true})
	})

	// GET /api/rules  {rule_devices:[...], advanced:[...]}
	mux.HandleFunc("/api/rules", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		rows, err := db.listRuleDevices()
		if err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		send(rw, 200, map[string]interface{}{"rule_devices": rows, "advanced": advancedRules()})
	})

	// POST /api/rule  {input_guid,input_value,output_guid,output_value}
	mux.HandleFunc("/api/rule", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		var r2 RuleDevice
		if err := json.NewDecoder(r.Body).Decode(&r2); err != nil {
			send(rw, 400, map[string]string{"error": "bad json"})
			return
		}
		if strings.TrimSpace(r2.InputGUID) == "" || strings.TrimSpace(r2.OutputGUID) == "" {
			send(rw, 400, map[string]string{"error": "input_guid and output_guid required"})
			return
		}
		if err := db.insertRuleDevice(r2); err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		reloadRules(st, db)
		log.Printf("[rule] added %s = %v -> %s = %v", r2.InputGUID, r2.InputValue, r2.OutputGUID, r2.OutputValue)
		send(rw, 200, map[string]bool{"ok": true})
	})

	// DELETE /api/rules/<id>
	mux.HandleFunc("/api/rules/", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodDelete {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		id, err := strconv.ParseInt(strings.TrimPrefix(r.URL.Path, "/api/rules/"), 10, 64)
		if err != nil {
			send(rw, 400, map[string]string{"error": "bad id"})
			return
		}
		if err := db.deleteRuleDevice(id); err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		reloadRules(st, db)
		log.Printf("[rule] deleted id %d", id)
		send(rw, 200, map[string]bool{"ok": true})
	})

	// floors/rooms (HIOTO scheme) for the dashboard + a registration app to
	// build pickers; mirrors the :8000 API.
	mux.HandleFunc("/api/floors", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		send(rw, 200, hiotoFloors)
	})
	mux.HandleFunc("/api/rooms", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		send(rw, 200, hiotoRooms)
	})

	// GET /api/devices/export.csv — download all devices as CSV (Excel-openable).
	mux.HandleFunc("/api/devices/export.csv", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		devs, err := db.listDevices()
		if err != nil {
			send(rw, 500, map[string]string{"error": err.Error()})
			return
		}
		rw.Header().Set("Content-Type", "text/csv; charset=utf-8")
		rw.Header().Set("Content-Disposition", "attachment; filename=devices.csv")
		_, _ = rw.Write([]byte{0xEF, 0xBB, 0xBF}) // UTF-8 BOM so Excel opens it correctly
		w := csv.NewWriter(rw)
		_ = w.Write(deviceCSVHeaders)
		for _, d := range devs {
			_ = w.Write([]string{
				d.GUID, d.MAC, d.Type, d.Name, d.Status, d.StatusDevice, d.Version, d.Minor,
				d.Category, strconv.Itoa(d.Quantity), strconv.Itoa(d.RoomID), strconv.Itoa(d.FloorID),
				strconv.FormatFloat(d.XPosition, 'f', -1, 64), strconv.FormatFloat(d.YPosition, 'f', -1, 64),
				d.LastSeen,
			})
		}
		w.Flush()
	})

	// POST /api/devices/import.csv — bulk register from CSV (Excel export).
	mux.HandleFunc("/api/devices/import.csv", func(rw http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			send(rw, 405, map[string]string{"error": "method not allowed"})
			return
		}
		rd := csv.NewReader(r.Body)
		rd.FieldsPerRecord = -1 // header drives the mapping; allow ragged rows
		rd.TrimLeadingSpace = true
		records, err := rd.ReadAll()
		if err != nil {
			send(rw, 400, map[string]string{"error": "bad csv: " + err.Error()})
			return
		}
		if len(records) == 0 {
			send(rw, 400, map[string]string{"error": "empty csv"})
			return
		}
		// strip a UTF-8 BOM from the first header cell (Excel writes one)
		if len(records[0]) > 0 {
			records[0][0] = strings.TrimPrefix(records[0][0], "\ufeff")
		}
		header := map[string]int{}
		for i, h := range records[0] {
			header[strings.ToLower(strings.TrimSpace(h))] = i
		}
		col := func(rec []string, name string) string {
			if i, ok := header[strings.ToLower(name)]; ok && i < len(rec) {
				return strings.TrimSpace(rec[i])
			}
			return ""
		}
		imported := 0
		for _, rec := range records[1:] {
			guid := strings.TrimLeft(strings.TrimSpace(col(rec, "guid")), ": \t")
			if guid == "" {
				continue
			}
			typ := col(rec, "type")
			if typ == "" {
				typ = "SENSOR"
			}
			qty, _ := strconv.Atoi(col(rec, "quantity"))
			room, _ := strconv.Atoi(col(rec, "room_id"))
			floor, _ := strconv.Atoi(col(rec, "floor_id"))
			xp, _ := strconv.ParseFloat(col(rec, "x_position"), 64)
			yp, _ := strconv.ParseFloat(col(rec, "y_position"), 64)
			dev := Device{
				GUID: guid, MAC: col(rec, "mac"), Type: typ, Name: col(rec, "name"),
				Status: col(rec, "status"), StatusDevice: col(rec, "status_device"),
				Version: col(rec, "version"), Minor: col(rec, "minor"), Category: col(rec, "category"),
				Quantity: qty, RoomID: room, FloorID: floor, XPosition: xp, YPosition: yp,
				LastSeen: col(rec, "last_seen"),
			}
			if err := db.upsertDevice(dev); err != nil {
				send(rw, 500, map[string]string{"error": err.Error()})
				return
			}
			imported++
		}
		send(rw, 200, map[string]int{"imported": imported})
	})

	registerTimerHandlers(mux, db, st)
	registerWebHandlers(mux, db, publish)

	addr := fmt.Sprintf(":%d", cfg.httpPort)
	log.Printf("device-management API on %s", addr)
	go func() {
		if err := http.ListenAndServe(addr, mux); err != nil {
			log.Fatalf("http: %v", err)
		}
	}()
}

func qrPNG(data string) ([]byte, error) {
	cmd := exec.Command("qrencode", "-o", "-", "-t", "PNG", "-s", "6", "-m", "2", data)
	return cmd.Output()
}

// reloadRules rebuilds the rule_devices-derived rules in the engine.
func reloadRules(st *state, db *DB) {
	rows, _ := db.listRuleDevices()
	st.mu.Lock()
	for name := range st.rules {
		if strings.HasPrefix(name, "rd-") {
			delete(st.rules, name)
			delete(st.runs, name)
		}
	}
	for _, r := range rulesFromRuleDevices(rows) {
		st.rules[r.Name] = r
		if _, ok := st.runs[r.Name]; !ok {
			st.runs[r.Name] = &ruleRun{}
		}
	}
	st.mu.Unlock()
}
