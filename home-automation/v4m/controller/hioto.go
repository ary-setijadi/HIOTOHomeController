package main

import (
	"encoding/json"
	"fmt"
	"log"
	"strings"
)

// HIOTO device seeds — one representative device per category (guids match the
// naming seen in the reference design: LAMP.*, SAKLAR.*, HIOTO-*, LSKK-*).
var hiotoDevices = []Device{
	{GUID: "LAMP.001", MAC: "80:7D:3A:00:00:01", Type: "AKTUATOR", Name: "Lamp 1"},
	{GUID: "SAKLAR.001", MAC: "80:7D:3A:00:00:02", Type: "SENSOR", Name: "Light Switch 1"},
	{GUID: "HIOTO-SECURITYCAMERA-001", MAC: "80:7D:3A:00:00:03", Type: "SENSOR_CAMERA", Name: "Security Camera 1"},
	{GUID: "HIOTO-SUHU-001", MAC: "80:7D:3A:00:00:04", Type: "SENSOR_SUHU", Name: "Temperature 1"},
	{GUID: "HIOTO-GAS-001", MAC: "80:7D:3A:00:00:05", Type: "SENSOR_GAS_DETECTOR", Name: "Gas Detector 1"},
	{GUID: "LSKK-HIOTO-WATERLEVEL", MAC: "80:7D:3A:00:00:06", Type: "SENSOR_WATER_TANK", Name: "Water Tank"},
	{GUID: "HIOTO-WEATHER-001", MAC: "80:7D:3A:00:00:07", Type: "SENSOR_WEATHER", Name: "Weather Station"},
	{GUID: "HIOTO-SMARTBELL", MAC: "80:7D:3A:00:00:08", Type: "SENSOR_BELL", Name: "Smart Bell"},
	{GUID: "HIOTO-SMARTRELAY", MAC: "80:7D:3A:00:00:09", Type: "SENSOR_SMART_RELAY", Name: "Master Relay"},
	{GUID: "HIOTO-SMARTSTEKER", MAC: "80:7D:3A:00:00:0A", Type: "SENSOR_SMART_PLUG", Name: "Smart Plug"},
	{GUID: "HIOTO-DIDO-001", MAC: "80:7D:3A:00:00:0B", Type: "DI_DO", Name: "DI/DO Module"},
}

func seedHiotoDevices(d *DB) {
	for _, dev := range hiotoDevices {
		_ = d.upsertDevice(dev)
	}
}

// seedHiotoRules adds the classic HIOTO mapping: switch -> lamp.
func seedHiotoRules(d *DB) {
	_ = d.insertRuleDevice(RuleDevice{InputGUID: "SAKLAR.001", InputValue: 1, OutputGUID: "LAMP.001", OutputValue: 1})
	_ = d.insertRuleDevice(RuleDevice{InputGUID: "SAKLAR.001", InputValue: 0, OutputGUID: "LAMP.001", OutputValue: 0})
}

// hiotoTopicBinds are the MQTT topics (as AMQP routing keys) HIOTO devices use.
var hiotoTopicBinds = []string{
	"Sensor", "Aktuator", "Status", "Log.#",
	"sensor_suhu.#", "sensor_water_tank.#", "sensor_gas_detector.#",
	"sensor_weather.#", "smart_bell.#",
}

// isHiotoDevice reports whether a guid uses the HIOTO wire convention (rather
// than the internal V4 home.* convention).
func isHiotoDevice(guid string) bool {
	return !strings.HasPrefix(guid, "ACT-") && !strings.HasPrefix(guid, "SNS-")
}

// isHiotoRoutingKey reports whether a routing key belongs to a HIOTO topic.
func isHiotoRoutingKey(rk string) bool {
	if rk == "Sensor" || rk == "Aktuator" || rk == "Status" {
		return true
	}
	for _, p := range []string{"sensor_suhu.", "sensor_water_tank.", "sensor_gas_detector.", "sensor_weather.", "smart_bell.", "Log."} {
		if strings.HasPrefix(rk, p) {
			return true
		}
	}
	return false
}

// parseHiotoMessage extracts guid + value from either the JSON form
// {"guid":"...","value":...} or the plain "guid#value" string (switches publish
// this, e.g. "e4681593-...#10").
func parseHiotoMessage(body []byte) (string, float64, bool) {
	var m map[string]interface{}
	if json.Unmarshal(body, &m) == nil {
		guid, _ := m["guid"].(string)
		if v, ok := m["value"].(float64); ok {
			return guid, v, true
		}
		if guid != "" {
			return guid, 0, false
		}
	}
	s := strings.TrimSpace(string(body))
	if i := strings.LastIndex(s, "#"); i > 0 {
		guid := s[:i]
		if v, ok := toFloat(s[i+1:]); ok {
			return guid, v, true
		}
	}
	return "", 0, false
}

// handleHioto processes one message on a HIOTO topic. `devMap` is the in-memory
// device registry (avoids a SQLite SELECT per message).
func handleHioto(routingKey string, body []byte, st *state, db *DB, devMap map[string]Device) {
	guid, value, hasValue := parseHiotoMessage(body)
	if guid == "" {
		return
	}
	dev, ok := devMap[guid]
	if !ok {
		return // unknown/unregistered device — ignore
	}

	if routingKey == "Status" {
		_ = db.touchDevice(guid)
		return
	}

	// telemetry: record primary value + any specific metrics present.
	if hasValue {
		recordTelemetry(guid, dev.Name, "value", value)
		recordLive(guid, dev.Kind, numericType(dev.Type), value, dev.Name, dev.Type)
		_ = db.updateDeviceStatus(guid, floatStr(value))
	}
	// Extract metrics from the top level AND a nested "value" object (smart
	// plug / smart relay publish {"value":{"voltage","current","power",...}}
	// on Log.<guid>).
	var m map[string]interface{}
	if json.Unmarshal(body, &m) == nil {
		metrics := []string{"temperature", "humidity", "voltage", "current", "power", "energy", "frequency", "pf"}
		for _, metric := range metrics {
			if v, ok := m[metric].(float64); ok {
				recordTelemetry(guid, dev.Name, metric, v)
			}
		}
		if vm, ok := m["value"].(map[string]interface{}); ok {
			for _, metric := range metrics {
				if v, ok := vm[metric].(float64); ok {
					recordTelemetry(guid, dev.Name, metric, v)
				}
			}
		}
	}
	_ = db.touchDevice(guid)

	// sensor readings feed the rule engine; actuator status ("Aktuator") is
	// feedback only.
	if routingKey != "Aktuator" && (dev.Kind == KindSensor || dev.Kind == KindHybrid) {
		if hasValue {
			st.putSensor(guid, value)
			kickRules() // fire switch→lamp rules immediately, no tick delay
			if routingKey == "Sensor" {
				log.Printf("[hioto] Sensor %s = %v", guid, value)
			}
		}
	}
}

// hiotoCommand builds the HIOTO actuator command payload. Devices expect the
// plain "guid#value" string (e.g. "5a119b76-...#1"), not a JSON object.
func hiotoCommand(guid string, value float64) []byte {
	return []byte(fmt.Sprintf("%s#%s", guid, floatStr(value)))
}

func logHiotoEvent(routingKey, guid string, m map[string]interface{}) {
	if routingKey == "smart_bell" || strings.HasPrefix(routingKey, "smart_bell") {
		log.Printf("[bell] press from %s", guid)
	}
}
