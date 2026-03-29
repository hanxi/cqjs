// Command example_callmusicurl demonstrates how to use the cqjs Go SDK
// to load a lx music source script and call callMusicUrl to get a playback URL.
//
// Usage:
//
//	go run ./cmd/example_callmusicurl -bin ./cqjs -script path/to/lx-music-source.js -prelude ../../js/lx_prelude.js
package main

import (
	"flag"
	"fmt"
	"log"
	"os"
	"time"

	cqjs "github.com/hanxi/cqjs/go"
)

func main() {
	binPath := flag.String("bin", "./cqjs", "path to the cqjs binary")
	scriptPath := flag.String("script", "", "path to the lx music source JS file")
	preludePath := flag.String("prelude", "", "path to lx_prelude.js (sets up globalThis.lx)")
	source := flag.String("source", "wy", "music source name (e.g. kw, kg, tx)")
	quality := flag.String("quality", "320k", "audio quality (e.g. 128k, 320k, flac)")
	songInfo := flag.String("song", `{"name":"屋顶","singer":"周杰伦、温岚、吴宗宪","songmid":"5257138"}`, "song info JSON")
	flag.Parse()

	if *scriptPath == "" {
		log.Fatal("please specify -script flag")
	}
	if *preludePath == "" {
		log.Fatal("please specify -prelude flag (path to lx_prelude.js)")
	}

	// Read lx_prelude.js content for environment init_code
	preludeCode, err := os.ReadFile(*preludePath)
	if err != nil {
		log.Fatalf("failed to read prelude file %s: %v", *preludePath, err)
	}

	server, err := cqjs.NewServer(*binPath)
	if err != nil {
		log.Fatal("failed to start cqjs:", err)
	}
	defer server.Close()

	// 1. Create a JS environment with lx_prelude.js as init_code.
	//    This sets up globalThis.lx (event handlers, lx.request, lx._dispatch, etc.)
	//    which the music source script depends on.
	envID := "lx-env"
	if err := server.CreateEnv(envID, cqjs.WithInitCode(string(preludeCode))); err != nil {
		log.Fatal("create env failed:", err)
	}
	fmt.Println("environment created:", envID)

	// 2. Load the lx music source script into the environment
	loadResp, err := server.SendRequest(&cqjs.Request{
		Type:  "eval_file",
		EnvID: envID,
		Path:  *scriptPath,
	}, 30*time.Second)
	if err != nil {
		log.Fatal("load script failed:", err)
	}
	fmt.Println("script loaded:", loadResp.ValueString())

	// 3. Wait for the "inited" event (music source initialization complete)
	select {
	case evt := <-server.Events():
		fmt.Printf("event received: %s, data: %s\n", evt.Name, string(evt.Data))
	case <-time.After(10 * time.Second):
		log.Fatal("timeout waiting for inited event")
	}

	// 4. Call callMusicUrl via eval: invoke lx._dispatch to trigger the
	//    registered "request" handler, which returns a Promise resolving
	//    to the playback URL. The dispatch result is sent back as a
	//    "dispatchResult" event.
	dispatchCode := fmt.Sprintf(
		`lx._dispatch("__callMusicUrl__", "request", {source: %q, action: "musicUrl", info: {musicInfo: %s, quality: %q, type: %q}})`,
		*source, *songInfo, *quality, *quality,
	)
	_, err = server.Eval(envID, dispatchCode)
	if err != nil {
		log.Fatal("dispatch callMusicUrl failed:", err)
	}

	// 5. Wait for the dispatchResult event containing the playback URL
	select {
	case evt := <-server.Events():
		fmt.Printf("dispatch result: %s, data: %s\n", evt.Name, string(evt.Data))
	case <-time.After(60 * time.Second):
		log.Fatal("timeout waiting for callMusicUrl result")
	}
}
