// Package cqjs provides a Go wrapper around the cqjs multi-environment
// JavaScript runtime server. It communicates with the cqjs subprocess
// via JSON Lines over stdin/stdout.
package cqjs

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"sync"
	"sync/atomic"
	"time"
)

// Default timeout for requests.
const DefaultTimeout = 30 * time.Second

// Server manages a cqjs subprocess and provides methods to interact with it.
type Server struct {
	cmd     *exec.Cmd
	stdin   io.WriteCloser
	stdout  io.ReadCloser
	stderr  io.ReadCloser
	mu      sync.Mutex       // protects stdin writes
	pending sync.Map         // map[string]chan *Response
	events  chan *Event       // environment-pushed events
	counter atomic.Int64     // request ID counter
	done    chan struct{}     // closed when readLoop exits
	binPath string
	closeOnce sync.Once
}

// NewServer creates and starts a cqjs subprocess.
// binPath is the path to the cqjs binary.
func NewServer(binPath string) (*Server, error) {
	cmd := exec.Command(binPath)

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("cqjs: stdin pipe: %w", err)
	}

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("cqjs: stdout pipe: %w", err)
	}

	stderr, err := cmd.StderrPipe()
	if err != nil {
		return nil, fmt.Errorf("cqjs: stderr pipe: %w", err)
	}

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("cqjs: start: %w", err)
	}

	s := &Server{
		cmd:     cmd,
		stdin:   stdin,
		stdout:  stdout,
		stderr:  stderr,
		events:  make(chan *Event, 64),
		done:    make(chan struct{}),
		binPath: binPath,
	}

	go s.readLoop()
	go s.drainStderr()

	return s, nil
}

// Close shuts down the cqjs subprocess gracefully.
// It sends an exit command, closes stdin, and waits for the process to exit.
func (s *Server) Close() error {
	var closeErr error
	s.closeOnce.Do(func() {
		// Try to send exit command (best effort)
		_ = s.sendRaw(&Request{
			ID:   s.nextID(),
			Type: "exit",
		})

		// Close stdin to signal EOF
		if err := s.stdin.Close(); err != nil {
			closeErr = err
		}

		// Wait for readLoop to finish
		<-s.done

		// Wait for process to exit
		if err := s.cmd.Wait(); err != nil {
			// Process may have already exited
			if closeErr == nil {
				closeErr = err
			}
		}
	})
	return closeErr
}

// CreateEnv creates a new JS runtime environment.
func (s *Server) CreateEnv(envID string, opts ...EnvOption) error {
	req := &Request{
		Type:  "create_env",
		EnvID: envID,
	}
	for _, opt := range opts {
		opt(req)
	}

	resp, err := s.SendRequest(req, DefaultTimeout)
	if err != nil {
		return err
	}
	if resp.Type == "error" {
		return fmt.Errorf("cqjs: create_env %s: %s", envID, resp.Message)
	}
	return nil
}

// Eval executes JavaScript code in the specified environment.
func (s *Server) Eval(envID, code string) (*Response, error) {
	return s.EvalWithTimeout(envID, code, DefaultTimeout)
}

// EvalWithTimeout executes JavaScript code with a custom timeout.
func (s *Server) EvalWithTimeout(envID, code string, timeout time.Duration) (*Response, error) {
	req := &Request{
		Type:  "eval",
		EnvID: envID,
		Code:  code,
	}

	resp, err := s.SendRequest(req, timeout)
	if err != nil {
		return nil, err
	}
	if resp.Type == "error" {
		return resp, fmt.Errorf("cqjs: eval error: %s", resp.Message)
	}
	return resp, nil
}

// DestroyEnv destroys a JS runtime environment.
func (s *Server) DestroyEnv(envID string) error {
	req := &Request{
		Type:  "destroy_env",
		EnvID: envID,
	}

	resp, err := s.SendRequest(req, DefaultTimeout)
	if err != nil {
		return err
	}
	if resp.Type == "error" {
		return fmt.Errorf("cqjs: destroy_env %s: %s", envID, resp.Message)
	}
	return nil
}

// ListEnvs returns a list of all active environment IDs.
func (s *Server) ListEnvs() ([]string, error) {
	req := &Request{
		Type: "list_envs",
	}

	resp, err := s.SendRequest(req, DefaultTimeout)
	if err != nil {
		return nil, err
	}
	if resp.Type == "error" {
		return nil, fmt.Errorf("cqjs: list_envs: %s", resp.Message)
	}

	var envs []string
	if resp.Value != nil {
		if err := json.Unmarshal(resp.Value, &envs); err != nil {
			return nil, fmt.Errorf("cqjs: list_envs unmarshal: %w", err)
		}
	}
	return envs, nil
}

// Events returns a read-only channel of environment-pushed events.
func (s *Server) Events() <-chan *Event {
	return s.events
}

// ValueString extracts the string value from a response.
// If the value is a JSON string (quoted), it unquotes it.
// Otherwise returns the raw JSON.
func (r *Response) ValueString() string {
	if r == nil || r.Value == nil {
		return ""
	}
	raw := string(r.Value)
	// Try to unmarshal as a JSON string
	var s string
	if err := json.Unmarshal(r.Value, &s); err == nil {
		return s
	}
	return raw
}

// --- internal ---

func (s *Server) nextID() string {
	return fmt.Sprintf("%d", s.counter.Add(1))
}

// SendRequest sends a raw request to the cqjs subprocess and waits for a response.
// This is the low-level API for sending any request type (eval, eval_file, dispatch, etc.).
// The request ID is automatically assigned.
func (s *Server) SendRequest(req *Request, timeout time.Duration) (*Response, error) {
	id := s.nextID()
	req.ID = id

	ch := make(chan *Response, 1)
	s.pending.Store(id, ch)
	defer s.pending.Delete(id)

	if err := s.sendRaw(req); err != nil {
		return nil, err
	}

	select {
	case resp := <-ch:
		return resp, nil
	case <-time.After(timeout):
		return nil, fmt.Errorf("cqjs: request %s timed out after %v", id, timeout)
	case <-s.done:
		return nil, errors.New("cqjs: server closed")
	}
}

func (s *Server) sendRaw(req *Request) error {
	data, err := json.Marshal(req)
	if err != nil {
		return fmt.Errorf("cqjs: marshal request: %w", err)
	}
	data = append(data, '\n')

	s.mu.Lock()
	defer s.mu.Unlock()

	_, err = s.stdin.Write(data)
	if err != nil {
		return fmt.Errorf("cqjs: write stdin: %w", err)
	}
	return nil
}

func (s *Server) readLoop() {
	defer close(s.done)

	scanner := bufio.NewScanner(s.stdout)
	// Allow up to 10MB per line
	scanner.Buffer(make([]byte, 0, 64*1024), 10*1024*1024)

	for scanner.Scan() {
		line := scanner.Bytes()
		if len(line) == 0 {
			continue
		}

		var resp Response
		if err := json.Unmarshal(line, &resp); err != nil {
			// Skip malformed lines
			continue
		}

		// If response has an ID, dispatch to pending request
		if resp.ID != "" {
			if ch, ok := s.pending.Load(resp.ID); ok {
				ch.(chan *Response) <- &resp
			}
			continue
		}

		// No ID — treat as event
		if resp.Type == "event" {
			evt := &Event{
				EnvID: resp.EnvID,
				Name:  resp.Name,
				Data:  resp.Data,
			}
			select {
			case s.events <- evt:
			default:
				// Drop event if channel is full
			}
		}
	}
}

func (s *Server) drainStderr() {
	// Forward subprocess stderr to os.Stderr for debugging
	buf := make([]byte, 4096)
	for {
		n, err := s.stderr.Read(buf)
		if n > 0 {
			os.Stderr.Write(buf[:n])
		}
		if err != nil {
			return
		}
	}
}
