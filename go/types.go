package cqjs

import "encoding/json"

// Request is a JSON Lines request sent to the cqjs subprocess via stdin.
type Request struct {
	ID          string `json:"id"`
	Type        string `json:"type"`
	EnvID       string `json:"env_id,omitempty"`
	Code        string `json:"code,omitempty"`     // eval: JS code to execute
	Filename    string `json:"filename,omitempty"` // eval: source filename for error stacks (default "<eval>")
	InitCode    string `json:"init_code,omitempty"`
	MemoryLimit int64  `json:"memory_limit,omitempty"`
	StackSize   int64  `json:"stack_size,omitempty"`
	Path        string `json:"path,omitempty"` // eval_file: file path to read and execute
}

// Response is a JSON Lines response received from the cqjs subprocess via stdout.
type Response struct {
	ID      string          `json:"id,omitempty"`
	Type    string          `json:"type"`
	EnvID   string          `json:"env_id,omitempty"`
	Value   json.RawMessage `json:"value,omitempty"`
	Message string          `json:"message,omitempty"`
	Name    string          `json:"name,omitempty"`
	Data    json.RawMessage `json:"data,omitempty"`
}

// Event represents an environment-pushed event (no id field).
type Event struct {
	EnvID string          `json:"env_id,omitempty"`
	Name  string          `json:"name"`
	Data  json.RawMessage `json:"data,omitempty"`
}

// EnvOption is a functional option for CreateEnv.
type EnvOption func(*Request)

// WithInitCode sets the initialization code for the environment.
func WithInitCode(code string) EnvOption {
	return func(r *Request) {
		r.InitCode = code
	}
}

// WithMemoryLimit sets the memory limit (in MB) for the environment.
func WithMemoryLimit(mb int64) EnvOption {
	return func(r *Request) {
		r.MemoryLimit = mb
	}
}

// WithStackSize sets the stack size (in MB) for the environment.
func WithStackSize(mb int64) EnvOption {
	return func(r *Request) {
		r.StackSize = mb
	}
}
