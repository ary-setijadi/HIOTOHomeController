package main

import (
	"math"
	"sort"
	"time"
)

// Summary is the aggregate quantity derived from all device readings.
type Summary struct {
	Ts             time.Time       `json:"ts"`
	Type           string          `json:"type"`
	DeviceCount    int             `json:"deviceCount"`
	AvgTemperature float64         `json:"avgTemperature"`
	AvgHumidity    float64         `json:"avgHumidity"`
	MinBattery     float64         `json:"minBattery"`
	MaxBattery     float64         `json:"maxBattery"`
	Devices        []DeviceReading `json:"devices"`
}

// DeviceNote is the per-device "relevant information" sent to each device.
type DeviceNote struct {
	Type            string    `json:"type"`
	Ts              time.Time `json:"ts"`
	Device          string    `json:"device"`
	AvgTemperature  float64   `json:"avgTemperature"`
	AvgHumidity     float64   `json:"avgHumidity"`
	YourTemperature float64   `json:"yourTemperature"`
	YourHumidity    float64   `json:"yourHumidity"`
	YourBattery     float64   `json:"yourBattery"`
}

func round2(v float64) float64 {
	return math.Round(v*100) / 100
}

// ComputeSummary aggregates a snapshot of readings into a Summary.
func ComputeSummary(snapshot map[string]DeviceReading) Summary {
	s := Summary{Ts: time.Now(), Type: "controller-summary", DeviceCount: len(snapshot)}
	if len(snapshot) == 0 {
		return s
	}

	var sumT, sumH float64
	var minB, maxB float64
	first := true
	devices := make([]DeviceReading, 0, len(snapshot))

	for _, r := range snapshot {
		devices = append(devices, r)
		sumT += r.Temperature
		sumH += r.Humidity
		if first || r.Battery < minB {
			minB = r.Battery
		}
		if first || r.Battery > maxB {
			maxB = r.Battery
		}
		first = false
	}

	n := float64(len(snapshot))
	s.AvgTemperature = round2(sumT / n)
	s.AvgHumidity = round2(sumH / n)
	s.MinBattery = round2(minB)
	s.MaxBattery = round2(maxB)

	sort.Slice(devices, func(i, j int) bool { return devices[i].Device < devices[j].Device })
	s.Devices = devices
	return s
}
