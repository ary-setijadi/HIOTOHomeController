package main

import (
	"encoding/json"
	"log"
	"net/http"
	"strconv"
	"strings"
	"time"
)

// TimerRule is a time-based automation rule persisted in SQLite. The three
// schedules are:
//   - "at"      : At="HH:MM"                -> fire Then once daily
//   - "window"  : From/To="HH:MM"           -> Then inside the window, Else outside
//   - "duration": ForMinutes>0 (+At, or now) -> Then, then Else after N minutes
type TimerRule struct {
	ID         int64    `json:"id"`
	Name       string   `json:"name"`
	Enabled    bool     `json:"enabled"`
	At         string   `json:"at"`
	From       string   `json:"from"`
	To         string   `json:"to"`
	ForMinutes int      `json:"for_minutes"`
	Then       []action `json:"then"`
	Else       []action `json:"else"`
}

// toRule converts a timer rule into the engine's rule representation.
func (t TimerRule) toRule() rule {
	r := rule{Name: "timer-" + strconv.FormatInt(t.ID, 10), Then: t.Then, Else: t.Else}
	switch {
	case t.From != "" || t.To != "":
		// daily window (level mode: Then in-window, Else out-of-window)
		r.When = condition{Type: "time", From: t.From, To: t.To}
	case t.ForMinutes > 0:
		// countdown (trigger mode + duration)
		r.Mode = "trigger"
		if t.At != "" {
			r.When = condition{Type: "time", At: t.At, ForMinutes: t.ForMinutes}
		} else {
			r.When = condition{Type: "now", ForMinutes: t.ForMinutes}
		}
	case t.At != "":
		// one-shot at a time (trigger mode)
		r.Mode = "trigger"
		r.When = condition{Type: "time", At: t.At}
	}
	return r
}

func boolToInt(b bool) int {
	if b {
		return 1
	}
	return 0
}

// ---- timer_rules persistence ----

func (d *DB) listTimerRules() ([]TimerRule, error) {
	rows, err := d.Query(`SELECT id, name, enabled, at_time, from_time, to_time, for_minutes, then_json, else_json FROM timer_rules ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []TimerRule
	for rows.Next() {
		var tr TimerRule
		var enabled int
		var thenJSON, elseJSON string
		if err := rows.Scan(&tr.ID, &tr.Name, &enabled, &tr.At, &tr.From, &tr.To, &tr.ForMinutes, &thenJSON, &elseJSON); err != nil {
			return nil, err
		}
		tr.Enabled = enabled != 0
		_ = json.Unmarshal([]byte(thenJSON), &tr.Then)
		_ = json.Unmarshal([]byte(elseJSON), &tr.Else)
		out = append(out, tr)
	}
	return out, nil
}

func (d *DB) insertTimerRule(tr TimerRule) (int64, error) {
	thenJSON, _ := json.Marshal(tr.Then)
	elseJSON, _ := json.Marshal(tr.Else)
	now := time.Now().UTC().Format(time.RFC3339)
	res, err := d.Exec(`INSERT INTO timer_rules (name, enabled, at_time, from_time, to_time, for_minutes, then_json, else_json, created_at, updated_at)
		VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		tr.Name, boolToInt(tr.Enabled), tr.At, tr.From, tr.To, tr.ForMinutes, string(thenJSON), string(elseJSON), now, now)
	if err != nil {
		return 0, err
	}
	return res.LastInsertId()
}

func (d *DB) updateTimerRule(tr TimerRule) error {
	thenJSON, _ := json.Marshal(tr.Then)
	elseJSON, _ := json.Marshal(tr.Else)
	now := time.Now().UTC().Format(time.RFC3339)
	_, err := d.Exec(`UPDATE timer_rules SET name=?, enabled=?, at_time=?, from_time=?, to_time=?, for_minutes=?, then_json=?, else_json=?, updated_at=? WHERE id=?`,
		tr.Name, boolToInt(tr.Enabled), tr.At, tr.From, tr.To, tr.ForMinutes, string(thenJSON), string(elseJSON), now, tr.ID)
	return err
}

func (d *DB) deleteTimerRule(id int64) error {
	_, err := d.Exec(`DELETE FROM timer_rules WHERE id = ?`, id)
	return err
}

// loadTimerRules (re)builds the timer-* rules in the engine from SQLite.
func loadTimerRules(db *DB, st *state) {
	rows, err := db.listTimerRules()
	if err != nil {
		log.Printf("[timer] list error: %v", err)
		return
	}
	st.mu.Lock()
	for name := range st.rules {
		if strings.HasPrefix(name, "timer-") {
			delete(st.rules, name)
			delete(st.runs, name)
		}
	}
	active := 0
	for _, tr := range rows {
		if !tr.Enabled {
			continue
		}
		r := tr.toRule()
		st.rules[r.Name] = r
		if _, ok := st.runs[r.Name]; !ok {
			st.runs[r.Name] = &ruleRun{}
		}
		active++
	}
	st.mu.Unlock()
	log.Printf("[timer] loaded %d timer rules (%d active)", len(rows), active)
}

// ---- timer rule HTTP API (:8081) ----

func registerTimerHandlers(mux *http.ServeMux, db *DB, st *state) {
	send := func(rw http.ResponseWriter, code int, obj interface{}) {
		rw.Header().Set("Content-Type", "application/json")
		rw.WriteHeader(code)
		_ = json.NewEncoder(rw).Encode(obj)
	}

	mux.HandleFunc("/api/timer-rules", func(rw http.ResponseWriter, r *http.Request) {
		switch r.Method {
		case http.MethodGet:
			rows, err := db.listTimerRules()
			if err != nil {
				send(rw, 500, map[string]string{"error": err.Error()})
				return
			}
			send(rw, 200, rows)
		case http.MethodPost:
			var tr TimerRule
			if err := json.NewDecoder(r.Body).Decode(&tr); err != nil {
				send(rw, 400, map[string]string{"error": "bad json"})
				return
			}
			tr.Name = strings.TrimSpace(tr.Name)
			if tr.Name == "" {
				send(rw, 400, map[string]string{"error": "name required"})
				return
			}
			id, err := db.insertTimerRule(tr)
			if err != nil {
				send(rw, 500, map[string]string{"error": err.Error()})
				return
			}
			tr.ID = id
			loadTimerRules(db, st)
			log.Printf("[timer] created #%d %q", id, tr.Name)
			send(rw, 200, tr)
		default:
			send(rw, 405, map[string]string{"error": "method not allowed"})
		}
	})

	mux.HandleFunc("/api/timer-rules/", func(rw http.ResponseWriter, r *http.Request) {
		rest := strings.TrimPrefix(r.URL.Path, "/api/timer-rules/")
		parts := strings.Split(rest, "/")
		if len(parts) == 0 || parts[0] == "" {
			send(rw, 404, map[string]string{"error": "not found"})
			return
		}
		id, err := strconv.ParseInt(parts[0], 10, 64)
		if err != nil {
			send(rw, 400, map[string]string{"error": "bad id"})
			return
		}
		switch {
		case r.Method == http.MethodDelete && len(parts) == 1:
			_ = db.deleteTimerRule(id)
			loadTimerRules(db, st)
			log.Printf("[timer] deleted #%d", id)
			send(rw, 200, map[string]bool{"ok": true})
		case r.Method == http.MethodPut && len(parts) == 1:
			var tr TimerRule
			if err := json.NewDecoder(r.Body).Decode(&tr); err != nil {
				send(rw, 400, map[string]string{"error": "bad json"})
				return
			}
			tr.ID = id
			tr.Name = strings.TrimSpace(tr.Name)
			if tr.Name == "" {
				send(rw, 400, map[string]string{"error": "name required"})
				return
			}
			if err := db.updateTimerRule(tr); err != nil {
				send(rw, 500, map[string]string{"error": err.Error()})
				return
			}
			loadTimerRules(db, st)
			log.Printf("[timer] updated #%d %q", id, tr.Name)
			send(rw, 200, tr)
		case r.Method == http.MethodPost && len(parts) == 2 && parts[1] == "toggle":
			rows, _ := db.listTimerRules()
			for _, tr := range rows {
				if tr.ID == id {
					tr.Enabled = !tr.Enabled
					_ = db.updateTimerRule(tr)
					loadTimerRules(db, st)
					send(rw, 200, tr)
					return
				}
			}
			send(rw, 404, map[string]string{"error": "not found"})
		default:
			send(rw, 404, map[string]string{"error": "not found"})
		}
	})
}
