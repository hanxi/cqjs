package cqjs_test

import (
	"os"
	"path/filepath"
	"runtime"
	"sync"
	"testing"
	"time"

	cqjs "github.com/hanxi/cqjs/go"
)

// binPath returns the path to the cqjs binary relative to this test file.
func binPath() string {
	// Try environment variable first
	if p := os.Getenv("CQJS_BIN"); p != "" {
		return p
	}
	// Default: assume binary is at ../cqjs relative to this test file
	_, filename, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(filename), "..", "cqjs")
}

func newServer(t *testing.T) *cqjs.Server {
	t.Helper()
	bin := binPath()
	if _, err := os.Stat(bin); os.IsNotExist(err) {
		t.Skipf("cqjs binary not found at %s, run 'make' in cqjs/ first", bin)
	}
	s, err := cqjs.NewServer(bin)
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}
	t.Cleanup(func() {
		_ = s.Close()
	})
	return s
}

func TestCreateAndDestroy(t *testing.T) {
	s := newServer(t)

	// Create environment
	err := s.CreateEnv("test_env_1")
	if err != nil {
		t.Fatalf("CreateEnv: %v", err)
	}

	// Verify it appears in list
	envs, err := s.ListEnvs()
	if err != nil {
		t.Fatalf("ListEnvs: %v", err)
	}
	found := false
	for _, e := range envs {
		if e == "test_env_1" {
			found = true
			break
		}
	}
	if !found {
		t.Errorf("expected test_env_1 in env list, got %v", envs)
	}

	// Destroy environment
	err = s.DestroyEnv("test_env_1")
	if err != nil {
		t.Fatalf("DestroyEnv: %v", err)
	}

	// Verify it's gone
	envs, err = s.ListEnvs()
	if err != nil {
		t.Fatalf("ListEnvs after destroy: %v", err)
	}
	for _, e := range envs {
		if e == "test_env_1" {
			t.Errorf("test_env_1 should not be in env list after destroy")
		}
	}
}

func TestEvalSimple(t *testing.T) {
	s := newServer(t)

	err := s.CreateEnv("eval_simple")
	if err != nil {
		t.Fatalf("CreateEnv: %v", err)
	}

	resp, err := s.Eval("eval_simple", "1 + 1")
	if err != nil {
		t.Fatalf("Eval: %v", err)
	}

	val := resp.ValueString()
	if val != "2" {
		t.Errorf("expected '2', got %q", val)
	}
}

func TestEvalWithInitCode(t *testing.T) {
	s := newServer(t)

	err := s.CreateEnv("eval_init", cqjs.WithInitCode("var x = 42;"))
	if err != nil {
		t.Fatalf("CreateEnv: %v", err)
	}

	resp, err := s.Eval("eval_init", "x + 1")
	if err != nil {
		t.Fatalf("Eval: %v", err)
	}

	val := resp.ValueString()
	if val != "43" {
		t.Errorf("expected '43', got %q", val)
	}
}

func TestMultipleEnvs(t *testing.T) {
	s := newServer(t)

	// Create two environments with different init code
	err := s.CreateEnv("env_a", cqjs.WithInitCode("var x = 100;"))
	if err != nil {
		t.Fatalf("CreateEnv env_a: %v", err)
	}

	err = s.CreateEnv("env_b", cqjs.WithInitCode("var x = 200;"))
	if err != nil {
		t.Fatalf("CreateEnv env_b: %v", err)
	}

	// Eval in env_a
	respA, err := s.Eval("env_a", "x")
	if err != nil {
		t.Fatalf("Eval env_a: %v", err)
	}
	if respA.ValueString() != "100" {
		t.Errorf("env_a: expected '100', got %q", respA.ValueString())
	}

	// Eval in env_b
	respB, err := s.Eval("env_b", "x")
	if err != nil {
		t.Fatalf("Eval env_b: %v", err)
	}
	if respB.ValueString() != "200" {
		t.Errorf("env_b: expected '200', got %q", respB.ValueString())
	}
}

func TestConcurrentEval(t *testing.T) {
	s := newServer(t)

	err := s.CreateEnv("conc_a", cqjs.WithInitCode("var counter = 0;"))
	if err != nil {
		t.Fatalf("CreateEnv conc_a: %v", err)
	}

	err = s.CreateEnv("conc_b", cqjs.WithInitCode("var counter = 1000;"))
	if err != nil {
		t.Fatalf("CreateEnv conc_b: %v", err)
	}

	const N = 5
	var wg sync.WaitGroup
	errCh := make(chan error, N*2)

	// Concurrent evals on env_a
	for i := 0; i < N; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			resp, err := s.Eval("conc_a", "counter++")
			if err != nil {
				errCh <- err
				return
			}
			_ = resp
		}()
	}

	// Concurrent evals on env_b
	for i := 0; i < N; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			resp, err := s.Eval("conc_b", "counter++")
			if err != nil {
				errCh <- err
				return
			}
			_ = resp
		}()
	}

	wg.Wait()
	close(errCh)

	for err := range errCh {
		t.Errorf("concurrent eval error: %v", err)
	}

	// Verify final counter values
	respA, err := s.Eval("conc_a", "counter")
	if err != nil {
		t.Fatalf("final eval conc_a: %v", err)
	}
	if respA.ValueString() != "5" {
		t.Errorf("conc_a counter: expected '5', got %q", respA.ValueString())
	}

	respB, err := s.Eval("conc_b", "counter")
	if err != nil {
		t.Fatalf("final eval conc_b: %v", err)
	}
	if respB.ValueString() != "1005" {
		t.Errorf("conc_b counter: expected '1005', got %q", respB.ValueString())
	}
}

func TestEvalError(t *testing.T) {
	s := newServer(t)

	err := s.CreateEnv("eval_err")
	if err != nil {
		t.Fatalf("CreateEnv: %v", err)
	}

	// Eval code that throws a ReferenceError
	_, err = s.Eval("eval_err", "undefinedVariable")
	if err == nil {
		t.Fatal("expected error for undefined variable, got nil")
	}
	t.Logf("Got expected error: %v", err)
}

func TestListEnvs(t *testing.T) {
	s := newServer(t)

	// Initially empty
	envs, err := s.ListEnvs()
	if err != nil {
		t.Fatalf("ListEnvs: %v", err)
	}
	if len(envs) != 0 {
		t.Errorf("expected 0 envs initially, got %d: %v", len(envs), envs)
	}

	// Create some environments
	for _, name := range []string{"list_a", "list_b", "list_c"} {
		if err := s.CreateEnv(name); err != nil {
			t.Fatalf("CreateEnv %s: %v", name, err)
		}
	}

	envs, err = s.ListEnvs()
	if err != nil {
		t.Fatalf("ListEnvs: %v", err)
	}
	if len(envs) != 3 {
		t.Errorf("expected 3 envs, got %d: %v", len(envs), envs)
	}
}

func TestDestroyNonExistent(t *testing.T) {
	s := newServer(t)

	err := s.DestroyEnv("nonexistent_env")
	if err == nil {
		t.Fatal("expected error when destroying non-existent env, got nil")
	}
	t.Logf("Got expected error: %v", err)
}

func TestServerClose(t *testing.T) {
	bin := binPath()
	if _, err := os.Stat(bin); os.IsNotExist(err) {
		t.Skipf("cqjs binary not found at %s", bin)
	}

	s, err := cqjs.NewServer(bin)
	if err != nil {
		t.Fatalf("NewServer: %v", err)
	}

	// Create an env to make sure server is working
	err = s.CreateEnv("close_test")
	if err != nil {
		t.Fatalf("CreateEnv: %v", err)
	}

	// Close the server
	err = s.Close()
	if err != nil {
		t.Logf("Close returned error (may be expected): %v", err)
	}

	// After close, operations should fail
	_, err = s.Eval("close_test", "1+1")
	if err == nil {
		t.Error("expected error after server close, got nil")
	}
}

func TestEvalWithTimeout(t *testing.T) {
	s := newServer(t)

	err := s.CreateEnv("timeout_test")
	if err != nil {
		t.Fatalf("CreateEnv: %v", err)
	}

	// Normal eval with short timeout should succeed
	resp, err := s.EvalWithTimeout("timeout_test", "1+2", 5*time.Second)
	if err != nil {
		t.Fatalf("EvalWithTimeout: %v", err)
	}
	if resp.ValueString() != "3" {
		t.Errorf("expected '3', got %q", resp.ValueString())
	}
}

func TestEvalStringResult(t *testing.T) {
	s := newServer(t)

	err := s.CreateEnv("str_test")
	if err != nil {
		t.Fatalf("CreateEnv: %v", err)
	}

	resp, err := s.Eval("str_test", "'hello world'")
	if err != nil {
		t.Fatalf("Eval: %v", err)
	}

	val := resp.ValueString()
	if val != "hello world" {
		t.Errorf("expected 'hello world', got %q", val)
	}
}
