package main

import (
	"encoding/json"
	"log"
	"net/http"
	"sort"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

var wsUpgrader = websocket.Upgrader{
	CheckOrigin:     func(r *http.Request) bool { return true },
	ReadBufferSize:  4096,
	WriteBufferSize: 4096,
}

var (
	wsMu      sync.Mutex
	wsClients = map[*websocket.Conn]bool{}
)

// broadcastWS pushes a text frame to every connected WebSocket client.
func broadcastWS(msg []byte) {
	wsMu.Lock()
	defer wsMu.Unlock()
	for c := range wsClients {
		_ = c.SetWriteDeadline(time.Now().Add(5 * time.Second))
		if err := c.WriteMessage(websocket.TextMessage, msg); err != nil {
			_ = c.Close()
			delete(wsClients, c)
		}
	}
}

func handleWS(db *DB, publish func(string, float64)) http.HandlerFunc {
	return func(rw http.ResponseWriter, r *http.Request) {
		conn, err := wsUpgrader.Upgrade(rw, r, nil)
		if err != nil {
			log.Printf("[ws] upgrade: %v", err)
			return
		}
		log.Printf("[ws] client connected")
		wsMu.Lock()
		wsClients[conn] = true
		wsMu.Unlock()
		defer func() {
			wsMu.Lock()
			delete(wsClients, conn)
			wsMu.Unlock()
			_ = conn.Close()
		}()

		// send a device snapshot on connect (alphabetical by name)
		if devs, err := db.listDevices(); err == nil {
			sort.Slice(devs, func(i, j int) bool { return devs[i].Name < devs[j].Name })
			out := make([]hiotoDevice, 0, len(devs))
			for i, d := range devs {
				out = append(out, hiotoDeviceDTO(i+1, d))
			}
			if b, err := json.Marshal(map[string]interface{}{
				"code": 200, "status": true, "message": "connected", "data": out,
			}); err == nil {
				_ = conn.WriteMessage(websocket.TextMessage, b)
			}
		}

		// read loop: log + parse control messages
		for {
			_, msg, err := conn.ReadMessage()
			if err != nil {
				return
			}
			log.Printf("[ws] recv: %s", string(msg))
			var m map[string]interface{}
			if json.Unmarshal(msg, &m) == nil {
				guid, _ := m["guid"].(string)
				if guid == "" {
					guid, _ = m["guid_aktuator"].(string)
				}
				if guid == "" {
					guid, _ = m["guid_device"].(string)
				}
				if v, ok := toFloat(m["value"]); ok && guid != "" {
					log.Printf("[ws] control %s = %v", guid, v)
					publish(guid, v)
				}
			}
		}
	}
}
