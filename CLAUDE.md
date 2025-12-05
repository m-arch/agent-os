# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Agent OS is a C++ AI-powered operating system assistant that bridges local LLMs with OS operations. It provides interactive AI interaction with file manipulation, command execution, and GUI capabilities.

## Build Commands

```bash
make all          # Compile all binaries (agent, agent-view, code-agent)
make agent        # Compile main agent binary only
make agent-view   # Compile GUI viewer only
make code-agent   # Compile code review agent only
make install      # Install binaries to /usr/local/bin/
make clean        # Remove compiled binaries
```

Build uses g++ with C++17, -O2 optimization, and links against libcurl, GTK3, and WebKit2GTK.

## Running

```bash
./start-agent.sh    # Start llama-server + agent (full stack)
./agent             # Run agent directly (requires llama-server on port 9090)
./code-agent        # Run code review agent (requires llama-server on port 9090)
./agent-view <url>  # Launch browser component with URL
```

## Architecture

Three executables with distinct responsibilities:

### agent (agent.cpp)
Main interactive assistant with comprehensive system integration:
- REPL interface with LLM communication via HTTP POST to localhost:9090/completion
- XML-based tool system:
  - `<read path="..."/>` - Read file contents
  - `<create path="...">content</create>` - Create files
  - `<edit path="..."><old>...</old><new>...</new></edit>` - Edit files
  - `<run>command</run>` - Execute shell commands
  - `<gui>html</gui>` - Display GUI content via agent-view
  - `<url>web_address</url>` - Open URLs in browser
- Conversation history with sliding window (max 8000 chars, trims at 6000)
- Spawns agent-view as subprocess for GUI display
- Configuration: MAX_TOKENS = 4096, TEMPERATURE = 0.7

### agent-view (agent-view.cpp)
WebKit2-based embedded browser for GUI display:
- Full browser with navigation toolbar (back, forward buttons) and JavaScript console
- Cookie persistence in ~/.agent_browser/
- History logging to ~/.agent_history
- Hardware acceleration disabled for stability
- GTK3 + WebKit2GTK integration

### code-agent (code-agent.cpp)
Specialized assistant for code modifications with review flow:
- Structured `<change>` block parsing with diff visualization
- Interactive accept/reject/edit workflow per change
- Lower temperature (0.3) for precise code generation
- Shorter context window (max 4000 chars, trims at 3000)
- Colored diff output for visual clarity

## Python Scripts

### transcript-listener.py

Background daemon that watches `/root/transcripts/` for changes and routes content to agents.

**Core Classes:**

- `PersistentAgent` - Manages long-running agent subprocesses with stdin/stdout communication
  - `start()` - Launch agent binary, start output reader thread
  - `send(text)` - Send input, wait for response (3-second stability check)
  - `stop()` - Gracefully terminate with "exit" command

- `AgentManager` - Orchestrates LLM server and multiple agents
  - `ensure_llm_server(port, model)` - Start/verify llama-server, blocks until healthy
  - `suspend_llm()` / `resume_llm()` - SIGSTOP/SIGCONT for VRAM management during VL tasks
  - `send_to_agent(name, text)` - Route to persistent agent, handles `[PROJECT:]` context injection
  - `send_to_claude(content)` - Route to Claude Code CLI with context persistence
  - `handle_capture(content)` - Suspend LLM, run VL processing, resume LLM

- `TranscriptWatcher` - File monitoring loop
  - `check_file(filename)` - Return new content if file modified after startup
  - `watch()` - Main loop, polls files every 1 second in priority order

**Key Configuration:**
```python
TRANSCRIPT_DIR = "/root/transcripts"
TEXT_MODEL = "~/workspace/models/qwen2.5-coder-14b-instruct-q4_k_m.gguf"
VL_MODEL = "~/workspace/models/Qwen3-VL-8B-Instruct-Q8_0.gguf"
TEXT_PORT = 9090
VL_PORT = 9091
MAX_LOG_ENTRIES = 50
```

**Claude Code Integration:**
- Context persistence via `/root/agent-logs/claude_context.log`
- Max context: 8000 chars (auto-trims older content)
- Commands: `reset` (clear context), `compact` (summarize context), `analyse <path>` (generate PROJECT_ANALYSIS.md)

### transcript-buttons.py

GTK3 Layer Shell overlay widget for voice-controlled agent interaction.

**Core Class: `TranscriptButtons`**

- Anchored to right edge of screen using GtkLayerShell
- Fixed width 390px with scrollable output area

**UI Components:**
- `btn_main` (red) - General assistant recording
- `btn_coding` (green) - Code agent with project folder selection via zenity
- `btn_capture` (blue) - Screenshot + voice capture workflow
- `btn_claude` (gray/purple) - Toggle Claude Code mode
- `btn_restart` (⟳) - Kill all processes, respawn widget
- `text_input` - Direct text input to agents
- `output_view` - Scrollable log (500 lines max)
- `sys_label` - CPU/RAM/GPU stats updated every 2 seconds

**Recording Flow:**
1. `on_button_clicked()` → `start_recording()` - spawns `arecord` subprocess
2. Click again → `stop_recording()` - SIGINT to arecord
3. `transcribe_thread()`:
   - Suspend llama-server (SIGSTOP) to free VRAM
   - Convert audio: ffmpeg to 16kHz mono
   - Run whisper-cli with GPU (-ng flag)
   - Prepend `[CLAUDE]` and/or `[PROJECT: path]` if applicable
   - Resume llama-server (SIGCONT)
4. Listener picks up transcript file change

**Capture Flow:**
1. `start_capture()` → `capture_screenshot_thread()`:
   - `slurp` for region selection
   - `grim -g region` saves PNG
   - Start `arecord` for audio
2. `stop_capture()` → `process_capture_thread()`:
   - Suspend llama-server, transcribe with whisper
   - If Claude mode: send image path + transcript to Claude CLI
   - If local mode: start VL server (port 9091), send base64 image + prompt
   - Parse `TARGET: coding/main` from VL response
   - Write to appropriate transcript file

**Key Functions:**
- `update_system_stats()` - Reads /proc/stat, /proc/meminfo, nvidia-smi
- `append_output(text)` - Thread-safe log append via GLib.idle_add
- `start_listener()` - Spawns transcript-listener.py, captures stdout
- `read_listener_output()` - Background thread reading listener output

**Dependencies:**
- gi.repository: Gtk, Gdk, GLib, GtkLayerShell, Pango
- External: whisper-cli, ffmpeg, arecord, slurp, grim, zenity, nvidia-smi

## Data Flow

```
User Input → Build Prompt → HTTP POST to llama-server:9090/completion
          → Parse Response → Execute XML Tools → Update History → Loop
```

## LLM Integration

All agents communicate with llama-cpp-server:
- **Text model**: Port 9090 (Qwen2.5-coder or Qwen3-Coder)
- **Vision model**: Port 9091 (Qwen3-VL-8B) - used for image/capture processing
- Request: JSON with prompt, n_predict (4096), temperature, stop tokens
- Response: JSON with "content" field
- Default models in ~/downloads/ or /root/workspace/models/

## Key Functions

**agent.cpp:**
- `query_llm()` - HTTP communication with LLM server
- `process_tools()` - Parse and execute XML tool tags from response
- `show_gui()` - Create temp HTML file and spawn agent-view

**code-agent.cpp:**
- `parse_changes()` - Extract change blocks from LLM response
- `show_diff()` - Display colored before/after comparison
- `apply_change()` - Apply validated changes to files

## Dependencies

Build: g++ (C++17), make, pkg-config, libcurl-dev, libgtk-3-dev, libwebkit2gtk-4.0-dev

Runtime: GTK3, WebKit2GTK, libcurl, llama-cpp-server (llama.cpp)

## Available Models

Located in /root/workspace/models/:
- qwen2.5-coder-14b-instruct-q4_k_m.gguf (9GB) - Main text model
- qwen2.5-coder-7b-instruct-q4_k_m.gguf (4.7GB) - Smaller text model
- Qwen3-Coder-30B-A3B-Instruct-Q3_K_M.gguf (14.7GB) - Large coder model
- Qwen3-VL-8B-Instruct-Q8_0.gguf (8.7GB) - Vision language model
