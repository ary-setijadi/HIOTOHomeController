package main

import (
	"sync"
	"time"
)

// DeviceReading is the latest telemetry snapshot for a single device.
type DeviceReading struct {
	Device      string    `json:"device"`
	Temperature float64   `json:"temperature"`
	Humidity    float64   `json:"humidity"`
	Battery     float64   `json:"battery"`
	LastSeen    time.Time `json:"lastSeen"`
	Source      string    `json:"source"`
}

// Store keeps the latest reading per device, keyed by device id.
type Store struct {
	mu      sync.RWMutex
	devices map[string]DeviceReading
}

func NewStore() *Store {
	return &Store{devices: make(map[string]DeviceReading)}
}

// Update records (or overwrites) the latest reading for a device.
func (s *Store) Update(r DeviceReading) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r.LastSeen = time.Now()
	s.devices[r.Device] = r
}

// Snapshot returns a copy of all current device readings.
func (s *Store) Snapshot() map[string]DeviceReading {
	s.mu.RLock()
	defer s.mu.RUnlock()
	out := make(map[string]DeviceReading, len(s.devices))
	for k, v := range s.devices {
		out[k] = v
	}
	return out
}
