package main

import "strings"

// HIOTO device model: categories observed in the live system (§8 of the
// reference design) plus the smart-plug category implied by log_smart_plugs.

// Categories is the canonical list of HIOTO device categories.
var Categories = []string{
	"AKTUATOR",             // lamps, smart plugs, door locks, sirens, DO modules
	"SENSOR",               // light switches, RFID, panic buttons, DI modules
	"SENSOR_CAMERA",        // security cameras
	"SENSOR_SUHU",          // temperature + humidity
	"SENSOR_GAS_DETECTOR",  // gas concentration
	"SENSOR_WATER_TANK",    // water level (cm)
	"SENSOR_WEATHER",       // weather station / anemometer
	"SENSOR_BELL",          // smart doorbell
	"SENSOR_SMART_RELAY",   // master relay + power metering
	"SENSOR_SMART_PLUG",    // smart plug + power metering
	"DI_DO",                // digital I/O module
	"DI/DO",                // digital I/O module (HIOTO spelling variant)
}

// Kind is how a device participates in the rule engine.
const (
	KindSensor   = "sensor"
	KindActuator = "actuator"
	KindHybrid   = "hybrid" // both reads telemetry and accepts commands
)

// ValueType is how a reading/command is represented.
const (
	ValueDigital = "digital" // 0/1 (or integer)
	ValueAnalog  = "analog"  // float
	ValueEvent   = "event"   // one-shot event (e.g. bell press)
	ValueCapture = "capture" // media capture (camera)
)

// kindOf maps a category to its rule-engine role.
func kindOf(category string) string {
	switch category {
	case "AKTUATOR":
		return KindActuator
	case "SENSOR_SMART_RELAY", "SENSOR_SMART_PLUG", "DI_DO", "DI/DO":
		return KindHybrid
	default:
		return KindSensor
	}
}

// valueTypeOf maps a category to its value representation.
func valueTypeOf(category string) string {
	switch category {
	case "SENSOR_SUHU", "SENSOR_GAS_DETECTOR", "SENSOR_WATER_TANK", "SENSOR_WEATHER":
		return ValueAnalog
	case "SENSOR_CAMERA":
		return ValueCapture
	case "SENSOR_BELL":
		return ValueEvent
	case "SENSOR_SMART_RELAY", "SENSOR_SMART_PLUG":
		return ValueAnalog // primary value = power/energy metering
	default:
		return ValueDigital
	}
}

// numericType returns the numeric device type used in topic routing
// (1 = digital, 2 = analog).
func numericType(category string) int {
	if valueTypeOf(category) == ValueAnalog {
		return 2
	}
	return 1
}

// floorLabel maps a device name to its building floor. HIOTO names embed the
// floor token (e.g. "LAMP.2-2ND.FLOOR-LAB AUTOMATION-246-02" → "Lantai 2");
// devices without one (weather, gas, smart plug, siren, DI/DO, ...) fall into
// "Lainnya".
func floorLabel(name string) string {
	up := strings.ToUpper(name)
	switch {
	case strings.Contains(up, "1ST"):
		return "Lantai 1"
	case strings.Contains(up, "2ND"):
		return "Lantai 2"
	case strings.Contains(up, "3RD"):
		return "Lantai 3"
	case strings.Contains(up, "4TH"):
		return "Lantai 4"
	}
	return "Lainnya"
}

// Device mirrors the registrations table and the full HIOTO registration
// scheme, so a dedicated mobile registration app can POST the same fields.
type Device struct {
	GUID         string  `json:"guid"`
	MAC          string  `json:"mac"`
	Type         string  `json:"type"` // HIOTO category
	Name         string  `json:"name"`
	Kind         string  `json:"kind"`
	ValueType    string  `json:"value_type"`
	Status       string  `json:"status"`
	StatusDevice string  `json:"status_device"`
	Version      string  `json:"version"`
	Minor        string  `json:"minor"`
	Category     string  `json:"category"`
	Quantity     int     `json:"quantity"`
	RoomID       int     `json:"room_id"`
	FloorID      int     `json:"floor_id"`
	XPosition    float64 `json:"x_position"`
	YPosition    float64 `json:"y_position"`
	LastSeen     string  `json:"last_seen"`
	CreatedAt    string  `json:"created_at"`
	UpdatedAt    string  `json:"updated_at"`
}
