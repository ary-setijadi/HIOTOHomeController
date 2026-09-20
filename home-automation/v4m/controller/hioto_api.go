package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"sort"
	"strconv"
	"strings"
)

// ---------------------------------------------------------------------------
// HIOTO-compatible REST API (separate port, default 8000) so the existing
// Android app can list + control V4m2 devices without modification.
// Mirrors the worker's /api/* routes and the {code,status,message,data} envelope.
// ---------------------------------------------------------------------------

type hiotoResp struct {
	Code    int         `json:"code"`
	Status  bool        `json:"status"`
	Message string      `json:"message"`
	Data    interface{} `json:"data"`
}

func hiotoJSON(rw http.ResponseWriter, httpCode int, msg string, data interface{}) {
	rw.Header().Set("Content-Type", "application/json")
	rw.WriteHeader(httpCode)
	_ = json.NewEncoder(rw).Encode(hiotoResp{Code: httpCode, Status: httpCode >= 200 && httpCode < 300, Message: msg, Data: data})
}

// ---- static floors/rooms (from the HIOTO AppData.db export) ----

type hiotoFloor struct {
	ID   int    `json:"id"`
	Name string `json:"name"`
}
type hiotoRoom struct {
	ID      int    `json:"id"`
	Name    string `json:"name"`
	FloorID int    `json:"floor_id"`
}

var hiotoFloors = []hiotoFloor{{ID: 1, Name: "Lantai 3"}, {ID: 2, Name: "Lantai 2"}, {ID: 3, Name: "Lantai 1"}, {ID: 4, Name: "Lantai 4"}}
var hiotoRooms = []hiotoRoom{
	{ID: 2, Name: "Teras", FloorID: 1}, {ID: 3, Name: "Kantor", FloorID: 2},
	{ID: 4, Name: "Storage", FloorID: 2}, {ID: 5, Name: "Lab Automation", FloorID: 2},
	{ID: 6, Name: "Kitchen", FloorID: 3}, {ID: 7, Name: "Garage Area", FloorID: 3},
	{ID: 8, Name: "Living Room", FloorID: 3}, {ID: 9, Name: "Bathroom", FloorID: 3},
	{ID: 10, Name: "Meeting Room", FloorID: 3}, {ID: 11, Name: "Meeting Room", FloorID: 2},
	{ID: 12, Name: "Terrace", FloorID: 2}, {ID: 13, Name: "Ruangan Zalfa", FloorID: 4},
}

// ---- response DTOs (HIOTO JSON shapes) ----

type hiotoDevice struct {
	ID           int         `json:"id"`
	GUID         string      `json:"guid"`
	MAC          string      `json:"mac"`
	Type         string      `json:"type"`
	Category     string      `json:"category"`
	Quantity     int         `json:"quantity"`
	Name         string      `json:"name"`
	Version      string      `json:"version"`
	Minor        string      `json:"minor"`
	Status       string      `json:"status"`
	StatusDevice string      `json:"status_device"`
	LastSeen     string      `json:"last_seen"`
	CreatedAt    string      `json:"created_at"`
	UpdatedAt    string      `json:"updated_at"`
	IDRoom       int         `json:"id_room"`
	Room         interface{} `json:"room"`
	IDFloor      interface{} `json:"id_floor"`
	Floor        interface{} `json:"floor"`
	XPosition    float64     `json:"x_position"`
	YPosition    float64     `json:"y_position"`
}

type hiotoRule struct {
	ID                  int64  `json:"id"`
	GuidSensor          string `json:"guid_sensor"`
	SensorName          string `json:"sensor_name"`
	SensorInputValue    string `json:"sensor_input_value"`
	GuidAktuator        string `json:"guid_aktuator"`
	AktuatorName        string `json:"aktuator_name"`
	AktuatorOutputValue string `json:"aktuator_output_value"`
	CreatedAt           string `json:"created_at"`
	UpdatedAt           string `json:"updated_at"`
}

func hiotoDeviceDTO(id int, d Device) hiotoDevice {
	cat := d.Category
	if cat == "" {
		cat = d.Type
	}
	version := d.Version
	if version == "" {
		version = "1.0"
	}
	minor := d.Minor
	if minor == "" {
		minor = "1.0"
	}
	roomID := d.RoomID
	floorID := d.FloorID
	// derive the floor from the room when it wasn't stored explicitly
	if floorID == 0 && roomID > 0 {
		if r := hiotoRoomByID(roomID); r != nil {
			floorID = r.FloorID
		}
	}
	var room, floor interface{}
	if r := hiotoRoomByID(roomID); r != nil {
		room = *r
	}
	if f := hiotoFloorByID(floorID); f != nil {
		floor = *f
	}
	var idFloor interface{}
	if floorID > 0 {
		idFloor = floorID
	}
	return hiotoDevice{
		ID: id, GUID: d.GUID, MAC: d.MAC, Type: d.Type, Category: cat,
		Quantity: d.Quantity, Name: d.Name, Version: version, Minor: minor,
		Status: d.Status, StatusDevice: d.StatusDevice, LastSeen: d.LastSeen,
		CreatedAt: d.CreatedAt, UpdatedAt: d.UpdatedAt,
		IDRoom: roomID, Room: room, IDFloor: idFloor, Floor: floor,
		XPosition: d.XPosition, YPosition: d.YPosition,
	}
}

func hiotoRoomByID(id int) *hiotoRoom {
	for i := range hiotoRooms {
		if hiotoRooms[i].ID == id {
			return &hiotoRooms[i]
		}
	}
	return nil
}

func hiotoFloorByID(id int) *hiotoFloor {
	for i := range hiotoFloors {
		if hiotoFloors[i].ID == id {
			return &hiotoFloors[i]
		}
	}
	return nil
}

// fmtSwitchVal renders an input/output value the way HIOTO does: 2-channel
// switches use "00"/"01"/"10"/"11", single-bit uses "0"/"1".
func fmtSwitchVal(val float64, twoBit bool) string {
	if twoBit {
		switch int(val) {
		case 0: return "00"
		case 1: return "01"
		case 2: return "10"
		default: return "11"
		}
	}
	return strconv.Itoa(int(val))
}

func startHiotoAPI(db *DB, st *state, cfg config, publish func(string, float64), deviceMap map[string]Device) {
	port := envIntOr("HIOTO_PORT", 8000)

	// Precompute which sensors are 2-channel (any rule input value 2 or 3).
	twoBit := map[string]bool{}
	if rows, err := db.listRuleDevices(); err == nil {
		for _, r := range rows {
			if r.InputValue >= 2 {
				twoBit[r.InputGUID] = true
			}
		}
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", func(rw http.ResponseWriter, r *http.Request) {
		log.Printf("[hioto-api] %s %s", r.Method, r.URL.Path)
		path := r.URL.Path

		// control is also exposed under several aliases (the app's exact route
		// varies by build); the body is parsed leniently below.
		if path == "/api/control" || path == "/api/device/control" || path == "/api/control-device" {
			if r.Method != http.MethodPost && r.Method != http.MethodPut {
				hiotoJSON(rw, 405, "method not allowed", nil)
				return
			}
			hiotoControl(rw, r, db, publish)
			return
		}

		switch {
		case path == "/api":
			hiotoJSON(rw, 200, "OK", map[string]string{"app": "hioto-worker", "impl": "v4m2"})

		case path == "/api/devices":
			if r.Method != http.MethodGet {
				hiotoJSON(rw, 405, "method not allowed", nil)
				return
			}
			devs, err := db.listDevices()
			if err != nil {
				hiotoJSON(rw, 500, "error", nil)
				return
			}
			sort.Slice(devs, func(i, j int) bool { return devs[i].Name < devs[j].Name })
			out := make([]hiotoDevice, 0, len(devs))
			for i, d := range devs {
				out = append(out, hiotoDeviceDTO(i+1, d))
			}
			hiotoJSON(rw, 200, "Success get all device", out)

		case path == "/api/rules":
			if r.Method != http.MethodGet {
				hiotoJSON(rw, 405, "method not allowed", nil)
				return
			}
			rows, err := db.listRuleDevices()
			if err != nil {
				hiotoJSON(rw, 500, "error", nil)
				return
			}
			out := make([]hiotoRule, 0, len(rows))
			for _, rd := range rows {
				out = append(out, hiotoRule{
					ID: rd.ID, GuidSensor: rd.InputGUID,
					SensorName:       nameOfDevice(deviceMap, rd.InputGUID),
					SensorInputValue: fmtSwitchVal(rd.InputValue, twoBit[rd.InputGUID]),
					GuidAktuator:     rd.OutputGUID,
					AktuatorName:     nameOfDevice(deviceMap, rd.OutputGUID),
					AktuatorOutputValue: fmtSwitchVal(rd.OutputValue, false),
				})
			}
			sort.Slice(out, func(i, j int) bool { return out[i].SensorName < out[j].SensorName })
			hiotoJSON(rw, 200, "success get all rules pagination", out)

		case path == "/api/floors":
			floors := append([]hiotoFloor{}, hiotoFloors...)
			sort.Slice(floors, func(i, j int) bool { return floors[i].Name < floors[j].Name })
			hiotoJSON(rw, 200, "Success get all floors", floors)

		case path == "/api/rooms":
			rooms := append([]hiotoRoom{}, hiotoRooms...)
			sort.Slice(rooms, func(i, j int) bool { return rooms[i].Name < rooms[j].Name })
			hiotoJSON(rw, 200, "Success get all rooms", rooms)

		case path == "/api/device":
			if r.Method == http.MethodPost {
				hiotoRegisterDevice(rw, r, db)
			} else {
				hiotoJSON(rw, 405, "method not allowed", nil)
			}

		case strings.HasPrefix(path, "/api/device/"):
			guid := strings.TrimPrefix(path, "/api/device/")
			if guid == "" {
				hiotoJSON(rw, 404, "not found", nil)
				return
			}
			switch r.Method {
			case http.MethodGet:
				d, ok := db.getDevice(guid)
				if !ok {
					hiotoJSON(rw, 404, "device not found", nil)
					return
				}
				hiotoJSON(rw, 200, "Success", hiotoDeviceDTO(1, d))
			case http.MethodPut:
				hiotoUpdateDevice(rw, r, db, guid, deviceMap)
			case http.MethodDelete:
				_ = db.deleteDevice(guid)
				hiotoJSON(rw, 200, "Success", map[string]bool{"ok": true})
			default:
				hiotoJSON(rw, 405, "method not allowed", nil)
			}

		case path == "/api/rule":
			switch r.Method {
			case http.MethodPost:
				hiotoCreateRule(rw, r, db, st)
			case http.MethodDelete:
				hiotoJSON(rw, 200, "Success", map[string]bool{"ok": true})
			default:
				hiotoJSON(rw, 405, "method not allowed", nil)
			}

		case path == "/api/ws":
			handleWS(db, publish)(rw, r)

		default:
			hiotoJSON(rw, 404, "not found", nil)
		}
	})

	addr := fmt.Sprintf(":%d", port)
	log.Printf("HIOTO-compatible API on %s", addr)
	go func() {
		if err := http.ListenAndServe(addr, mux); err != nil {
			log.Fatalf("hioto http: %v", err)
		}
	}()
}

func nameOfDevice(deviceMap map[string]Device, guid string) string {
	if d, ok := deviceMap[guid]; ok && d.Name != "" {
		return d.Name
	}
	return guid
}

// hiotoControl parses a lenient control body and publishes an actuator command.
func hiotoControl(rw http.ResponseWriter, r *http.Request, db *DB, publish func(string, float64)) {
	body, _ := io.ReadAll(r.Body)
	log.Printf("[hioto-api] control body: %s", string(body))
	var m map[string]interface{}
	if err := json.Unmarshal(body, &m); err != nil {
		hiotoJSON(rw, 400, "bad json", nil)
		return
	}
	guid := firstStr(m, "guid", "guid_aktuator", "device_guid", "guid_device", "guid_akt")
	val, ok := firstNum(m, "value", "status", "output_value", "aktuator_output_value", "device_value")

	// The Android app sends {"type":"AKTUATOR","message":"<guid>#<value>"}.
	if guid == "" || !ok {
		if msg, isStr := m["message"].(string); isStr && msg != "" {
			parts := strings.SplitN(msg, "#", 2)
			if len(parts) == 2 {
				guid = strings.TrimSpace(parts[0])
				if f, conv := toFloat(parts[1]); conv {
					val, ok = f, true
				}
			}
		}
	}

	if guid == "" {
		hiotoJSON(rw, 400, "guid required", nil)
		return
	}
	if !ok {
		hiotoJSON(rw, 400, "value required", nil)
		return
	}
	if _, known := db.getDevice(guid); !known {
		hiotoJSON(rw, 404, "device not found", nil)
		return
	}
	publish(guid, val)
	hiotoJSON(rw, 200, "Success control device", map[string]interface{}{"guid": guid, "value": val})
}

func hiotoRegisterDevice(rw http.ResponseWriter, r *http.Request, db *DB) {
	var dev Device
	if err := json.NewDecoder(r.Body).Decode(&dev); err != nil {
		hiotoJSON(rw, 400, "bad json", nil)
		return
	}
	dev.GUID = strings.TrimSpace(dev.GUID)
	if dev.GUID == "" {
		hiotoJSON(rw, 400, "guid required", nil)
		return
	}
	if dev.Type == "" {
		dev.Type = "SENSOR"
	}
	_ = db.upsertDevice(dev)
	hiotoJSON(rw, 200, "Success register device", map[string]string{"guid": dev.GUID})
}

func hiotoUpdateDevice(rw http.ResponseWriter, r *http.Request, db *DB, guid string, deviceMap map[string]Device) {
	var b struct {
		MAC  string `json:"mac"`
		Type string `json:"type"`
		Name string `json:"name"`
	}
	if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
		hiotoJSON(rw, 400, "bad json", nil)
		return
	}
	if b.Type == "" {
		b.Type = "SENSOR"
	}
	_ = db.updateDevice(guid, b.Name, b.Type, b.MAC)
	// Keep the in-memory registry in sync so live broadcasts don't revert it.
	if dev, ok := deviceMap[guid]; ok {
		dev.Name = b.Name
		dev.Type = b.Type
		dev.MAC = b.MAC
		deviceMap[guid] = dev
	}
	hiotoJSON(rw, 200, "Success Update Device", map[string]string{"guid": guid})
}

func hiotoCreateRule(rw http.ResponseWriter, r *http.Request, db *DB, st *state) {
	var b struct {
		GuidSensor          string      `json:"guid_sensor"`
		SensorInputValue    interface{} `json:"sensor_input_value"`
		GuidAktuator        string      `json:"guid_aktuator"`
		AktuatorOutputValue interface{} `json:"aktuator_output_value"`
		// also accept the v4-style names
		InputGUID   string      `json:"input_guid"`
		InputValue  interface{} `json:"input_value"`
		OutputGUID  string      `json:"output_guid"`
		OutputValue interface{} `json:"output_value"`
	}
	if err := json.NewDecoder(r.Body).Decode(&b); err != nil {
		hiotoJSON(rw, 400, "bad json", nil)
		return
	}
	inGUID := firstNonEmpty(b.GuidSensor, b.InputGUID)
	outGUID := firstNonEmpty(b.GuidAktuator, b.OutputGUID)
	inVal, ok1 := toFloat(b.SensorInputValue)
	if !ok1 {
		inVal, ok1 = toFloat(b.InputValue)
	}
	outVal, ok2 := toFloat(b.AktuatorOutputValue)
	if !ok2 {
		outVal, ok2 = toFloat(b.OutputValue)
	}
	if inGUID == "" || outGUID == "" || !ok1 || !ok2 {
		hiotoJSON(rw, 400, "guid_sensor, sensor_input_value, guid_aktuator, aktuator_output_value required", nil)
		return
	}
	_ = db.insertRuleDevice(RuleDevice{InputGUID: inGUID, InputValue: inVal, OutputGUID: outGUID, OutputValue: outVal})
	reloadRules(st, db)
	hiotoJSON(rw, 200, "Success create rules", map[string]bool{"ok": true})
}

// ---- small helpers ----

func firstStr(m map[string]interface{}, keys ...string) string {
	for _, k := range keys {
		if v, ok := m[k].(string); ok && strings.TrimSpace(v) != "" {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

func firstNum(m map[string]interface{}, keys ...string) (float64, bool) {
	for _, k := range keys {
		if v, ok := toFloat(m[k]); ok {
			return v, true
		}
	}
	return 0, false
}

func firstNonEmpty(vals ...string) string {
	for _, v := range vals {
		if strings.TrimSpace(v) != "" {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

func toFloat(v interface{}) (float64, bool) {
	switch t := v.(type) {
	case float64:
		return t, true
	case float32:
		return float64(t), true
	case int:
		return float64(t), true
	case int64:
		return float64(t), true
	case json.Number:
		f, err := t.Float64()
		return f, err == nil
	case string:
		// bit-strings "00"/"01"/"10"/"11" parse as binary
		if n, err := strconv.ParseInt(t, 2, 64); err == nil {
			return float64(n), true
		}
		if f, err := strconv.ParseFloat(t, 64); err == nil {
			return f, true
		}
	}
	return 0, false
}
