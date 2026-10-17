package main

// Helpers for presence-based partial device updates (PUT /api/devices/:guid).
// A key present in the JSON body overwrites the field; an absent key leaves the
// current value untouched. JSON numbers arrive as float64.

func setStr(dst *string, v interface{}) {
	if s, ok := v.(string); ok {
		*dst = s
	}
}

func setInt(dst *int, v interface{}) {
	if f, ok := v.(float64); ok {
		*dst = int(f)
	}
}

func setFloat(dst *float64, v interface{}) {
	if f, ok := v.(float64); ok {
		*dst = f
	}
}
