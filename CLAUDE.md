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
File watcher that monitors transcript files and routes them to agents:
- Maintains single-instance lock via `/tmp/transcript-listener.lock`
- Monitors three transcript files: `main.txt`, `coding.txt`, `capture.txt`
- Two LLM servers: Text model (port 9090), Vision model (port 9091)
- Routes inputs to appropriate agent based on file
- Maintains action logs in JSON format (last 50 entries for context)

### transcript-buttons.py
GUI button interface for transcript interaction with the agents.

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
