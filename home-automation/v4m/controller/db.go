package main

import (
	"database/sql"
	"strconv"
	"sync"
	"time"

	_ "modernc.org/sqlite"
)

// DB wraps the SQLite handle. MaxOpenConns(1) is required: SQLite has a
// single-writer model and modernc.org/sqlite serializes access.
type DB struct {
	*sql.DB
}

// Telemetry write throttle: at most one write per (guid,metric) per interval,
// so a fast message stream can't drown the SQLite writer (the SD card is the
// real bottleneck on a 512 MB Orange Pi).
const throttleInterval = 5 * time.Second

var (
	throttleMu sync.Mutex
	throttle   = map[string]time.Time{}
)

func throttled(key string) bool {
	throttleMu.Lock()
	defer throttleMu.Unlock()
	now := time.Now()
	if last, ok := throttle[key]; ok && now.Sub(last) < throttleInterval {
		return false
	}
	throttle[key] = now
	return true
}

func openDB(path string) (*DB, error) {
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, err
	}
	db.SetMaxOpenConns(1)
	_, _ = db.Exec(`PRAGMA journal_mode=WAL; PRAGMA busy_timeout=5000; PRAGMA synchronous=NORMAL;`)
	if err := migrate(db); err != nil {
		return nil, err
	}
	return &DB{db}, nil
}

func migrate(db *sql.DB) error {
	schema := `
CREATE TABLE IF NOT EXISTS registrations (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  guid TEXT UNIQUE NOT NULL,
  mac TEXT,
  type TEXT NOT NULL,              -- HIOTO category (AKTUATOR, SENSOR, ...)
  name TEXT,
  status TEXT,
  status_device TEXT,
  version TEXT,
  minor TEXT,
  quantity INTEGER DEFAULT 0,
  last_seen TEXT,
  room_id INTEGER,
  floor_id INTEGER,
  category TEXT,
  x_position REAL DEFAULT 0,
  y_position REAL DEFAULT 0,
  created_at TEXT,
  updated_at TEXT
);

CREATE TABLE IF NOT EXISTS rule_devices (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  input_guid TEXT,
  input_value TEXT,
  output_guid TEXT,
  output_value TEXT,
  created_at TEXT,
  updated_at TEXT
);

CREATE TABLE IF NOT EXISTS alert_rules (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_guid TEXT,
  metric TEXT,
  operator TEXT,
  threshold REAL,
  message TEXT,
  cooldown_minutes INTEGER,
  is_active INTEGER DEFAULT 1,
  last_triggered_at TEXT,
  created_at TEXT,
  updated_at TEXT
);

CREATE TABLE IF NOT EXISTS timer_rules (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  enabled INTEGER DEFAULT 1,
  at_time TEXT,
  from_time TEXT,
  to_time TEXT,
  for_minutes INTEGER DEFAULT 0,
  then_json TEXT,
  else_json TEXT,
  created_at TEXT,
  updated_at TEXT
);

CREATE TABLE IF NOT EXISTS logs (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_guid TEXT,
  metric TEXT,
  value REAL,
  unit TEXT,
  received_at TEXT
);
CREATE INDEX IF NOT EXISTS idx_logs_guid ON logs(device_guid);
CREATE INDEX IF NOT EXISTS idx_logs_time ON logs(received_at);
`
	_, err := db.Exec(schema)
	if err != nil {
		return err
	}
	// Additive migration for databases created by earlier versions (the ALTER
	// fails harmlessly with "duplicate column" when the column already exists).
	for _, col := range []struct{ name, def string }{
		{"version", "TEXT"},
		{"minor", "TEXT"},
		{"quantity", "INTEGER DEFAULT 0"},
		{"x_position", "REAL DEFAULT 0"},
		{"y_position", "REAL DEFAULT 0"},
	} {
		_, _ = db.Exec("ALTER TABLE registrations ADD COLUMN " + col.name + " " + col.def)
	}
	return nil
}

// ---- device registry ----

func (d *DB) listDevices() ([]Device, error) {
	rows, err := d.Query(`SELECT guid, mac, type, name,
		COALESCE(status,''), COALESCE(status_device,''), COALESCE(version,''), COALESCE(minor,''),
		COALESCE(category,''), COALESCE(quantity,0), COALESCE(room_id,0), COALESCE(floor_id,0),
		COALESCE(x_position,0), COALESCE(y_position,0), COALESCE(last_seen,''), COALESCE(created_at,''), COALESCE(updated_at,'')
		FROM registrations ORDER BY guid`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Device
	for rows.Next() {
		var dv Device
		if err := rows.Scan(&dv.GUID, &dv.MAC, &dv.Type, &dv.Name,
			&dv.Status, &dv.StatusDevice, &dv.Version, &dv.Minor,
			&dv.Category, &dv.Quantity, &dv.RoomID, &dv.FloorID,
			&dv.XPosition, &dv.YPosition, &dv.LastSeen, &dv.CreatedAt, &dv.UpdatedAt); err != nil {
			return nil, err
		}
		dv.Kind = kindOf(dv.Type)
		dv.ValueType = valueTypeOf(dv.Type)
		out = append(out, dv)
	}
	return out, nil
}

func (d *DB) countDevices() int {
	var n int
	_ = d.QueryRow(`SELECT COUNT(*) FROM registrations`).Scan(&n)
	return n
}

func (d *DB) upsertDevice(dev Device) error {
	now := time.Now().UTC().Format(time.RFC3339)
	cat := dev.Category
	if cat == "" {
		cat = dev.Type
	}
	_, err := d.Exec(`
INSERT INTO registrations (guid, mac, type, name, status, status_device, version, minor, category, quantity, room_id, floor_id, x_position, y_position, last_seen, created_at, updated_at)
VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
ON CONFLICT(guid) DO UPDATE SET
  mac=excluded.mac, type=excluded.type, name=excluded.name, status=excluded.status,
  status_device=excluded.status_device, version=excluded.version, minor=excluded.minor,
  category=excluded.category, quantity=excluded.quantity, room_id=excluded.room_id,
  floor_id=excluded.floor_id, x_position=excluded.x_position, y_position=excluded.y_position,
  last_seen=excluded.last_seen, updated_at=excluded.updated_at`,
		dev.GUID, dev.MAC, dev.Type, dev.Name, dev.Status, dev.StatusDevice, dev.Version, dev.Minor,
		cat, dev.Quantity, dev.RoomID, dev.FloorID, dev.XPosition, dev.YPosition, dev.LastSeen, now, now)
	return err
}

func (d *DB) deleteDevice(guid string) error {
	_, err := d.Exec(`DELETE FROM registrations WHERE guid = ?`, guid)
	return err
}

// updateDevice changes the mutable fields (name/type/mac) of a registration.
func (d *DB) updateDevice(guid, name, typ, mac string) error {
	now := time.Now().UTC().Format(time.RFC3339)
	_, err := d.Exec(`UPDATE registrations SET name = ?, type = ?, category = ?, mac = ?, updated_at = ? WHERE guid = ?`,
		name, typ, typ, mac, now, guid)
	return err
}

// updateDeviceStatus records the device's current state — the authoritative
// "status" that sensors (toggle info) and actuators (state/problem indicator)
// follow.
func (d *DB) updateDeviceStatus(guid, status string) error {
	now := time.Now().UTC().Format(time.RFC3339)
	_, err := d.Exec(`UPDATE registrations SET status = ?, updated_at = ? WHERE guid = ?`, status, now, guid)
	return err
}

// clearDevices removes every registration (the "clean all devices" operation).
func (d *DB) clearDevices() error {
	_, err := d.Exec(`DELETE FROM registrations`)
	return err
}

// importDevices upserts a batch of registrations (the "import from HIOTO" path).
func (d *DB) importDevices(devs []Device) error {
	for _, dev := range devs {
		if err := d.upsertDevice(dev); err != nil {
			return err
		}
	}
	return nil
}

func (d *DB) getDevice(guid string) (Device, bool) {
	row := d.QueryRow(`SELECT guid, mac, type, name,
		COALESCE(status,''), COALESCE(status_device,''), COALESCE(version,''), COALESCE(minor,''),
		COALESCE(category,''), COALESCE(quantity,0), COALESCE(room_id,0), COALESCE(floor_id,0),
		COALESCE(x_position,0), COALESCE(y_position,0), COALESCE(last_seen,''), COALESCE(created_at,''), COALESCE(updated_at,'')
		FROM registrations WHERE guid = ?`, guid)
	var dv Device
	if err := row.Scan(&dv.GUID, &dv.MAC, &dv.Type, &dv.Name,
		&dv.Status, &dv.StatusDevice, &dv.Version, &dv.Minor,
		&dv.Category, &dv.Quantity, &dv.RoomID, &dv.FloorID,
		&dv.XPosition, &dv.YPosition, &dv.LastSeen, &dv.CreatedAt, &dv.UpdatedAt); err != nil {
		return Device{}, false
	}
	dv.Kind = kindOf(dv.Type)
	dv.ValueType = valueTypeOf(dv.Type)
	return dv, true
}

func (d *DB) touchDevice(guid string) error {
	if !throttled("touch:" + guid) {
		return nil
	}
	now := time.Now().UTC().Format(time.RFC3339)
	_, err := d.Exec(`UPDATE registrations SET last_seen = ?, updated_at = ? WHERE guid = ?`, now, now, guid)
	return err
}

// ---- rules (rule_devices) ----

type RuleDevice struct {
	ID          int64   `json:"id"`
	InputGUID   string  `json:"input_guid"`
	InputValue  float64 `json:"input_value"`
	OutputGUID  string  `json:"output_guid"`
	OutputValue float64 `json:"output_value"`
}

func (d *DB) listRuleDevices() ([]RuleDevice, error) {
	rows, err := d.Query(`SELECT id, input_guid, input_value, output_guid, output_value FROM rule_devices ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []RuleDevice
	for rows.Next() {
		var r RuleDevice
		var iv, ov string
		if err := rows.Scan(&r.ID, &r.InputGUID, &iv, &r.OutputGUID, &ov); err != nil {
			return nil, err
		}
		r.InputValue = parseRuleValue(iv)
		r.OutputValue = parseRuleValue(ov)
		out = append(out, r)
	}
	return out, nil
}

func (d *DB) insertRuleDevice(r RuleDevice) error {
	now := time.Now().UTC().Format(time.RFC3339)
	_, err := d.Exec(`
INSERT INTO rule_devices (input_guid, input_value, output_guid, output_value, created_at, updated_at)
VALUES (?, ?, ?, ?, ?, ?)`,
		r.InputGUID, floatStr(r.InputValue), r.OutputGUID, floatStr(r.OutputValue), now, now)
	return err
}

func (d *DB) clearRuleDevices() error {
	_, err := d.Exec(`DELETE FROM rule_devices`)
	return err
}

// deleteRuleDevice removes a single rule_devices row by primary key.
func (d *DB) deleteRuleDevice(id int64) error {
	_, err := d.Exec(`DELETE FROM rule_devices WHERE id = ?`, id)
	return err
}

func (d *DB) countRuleDevices() int {
	var n int
	_ = d.QueryRow(`SELECT COUNT(*) FROM rule_devices`).Scan(&n)
	return n
}

// importRuleDevices replaces the rule_devices table with the given rows (the
// "capture from HIOTO rule_devices" path — feed it a JSON export of the table).
func (d *DB) importRuleDevices(rows []RuleDevice) error {
	if err := d.clearRuleDevices(); err != nil {
		return err
	}
	for _, r := range rows {
		if err := d.insertRuleDevice(r); err != nil {
			return err
		}
	}
	return nil
}

// ---- telemetry ----

func (d *DB) writeLog(guid, metric string, value float64, unit string) error {
	if !throttled(guid + "|" + metric) {
		return nil
	}
	_, err := d.Exec(`INSERT INTO logs (device_guid, metric, value, unit, received_at) VALUES (?, ?, ?, ?, ?)`,
		guid, metric, value, unit, time.Now().UTC().Format(time.RFC3339))
	return err
}

func (d *DB) countLogs() int {
	var n int
	_ = d.QueryRow(`SELECT COUNT(*) FROM logs`).Scan(&n)
	return n
}

// pruneLogs keeps only the newest `keep` telemetry rows, bounding the legacy
// SQLite logs table. (Telemetry now flows to the file-based bounded store, but
// rows written by earlier versions may still be present.)
func (d *DB) pruneLogs(keep int) error {
	_, err := d.Exec(`DELETE FROM logs WHERE id NOT IN (SELECT id FROM logs ORDER BY id DESC LIMIT ?)`, keep)
	return err
}

// ---- helpers ----

func parseFloat(s string) float64 {
	f, _ := strconv.ParseFloat(s, 64)
	return f
}

// parseRuleValue parses a rule_devices input/output value. HIOTO switch values
// are bit-strings ("0","1","00","01","10","11"); those are parsed as binary so
// a 2-channel switch's "10" -> 2 and "11" -> 3 (matching the integer the
// simulator publishes). Non-binary strings fall back to decimal (analog).
func parseRuleValue(s string) float64 {
	if v, err := strconv.ParseInt(s, 2, 64); err == nil {
		return float64(v)
	}
	return parseFloat(s)
}

func floatStr(f float64) string {
	return strconv.FormatFloat(f, 'f', -1, 64)
}
