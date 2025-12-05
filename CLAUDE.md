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
./start-agent.sh  # Start llama-server + agent (full stack)
./agent           # Run agent directly (requires llama-server on port 8080)
./code-agent      # Run code review agent
./agent-view <url>  # Launch browser component with URL
```

## Architecture

Three executables with distinct responsibilities:

### agent (agent.cpp)
Main interactive assistant with comprehensive system integration:
- REPL interface with LLM communication via HTTP POST to localhost:8080/completion
- XML-based tool system: `<read>`, `<create>`, `<edit>`, `<run>`, `<gui>`, `<url>`
- Conversation history with sliding window (max 8000 chars, trims at 6000)
- Spawns agent-view as subprocess for GUI display

### agent-view (agent-view.cpp)
WebKit2-based embedded browser for GUI display:
- Full browser with navigation toolbar and JavaScript console
- Cookie persistence in ~/.agent_browser/
- History logging to ~/.agent_history
- Hardware acceleration disabled for stability

### code-agent (code-agent.cpp)
Specialized assistant for code modifications with review flow:
- Structured `<change>` block parsing with diff visualization
- Interactive accept/reject/edit workflow per change
- Lower temperature (0.3) for precise code generation
- Shorter context window (max 4000 chars, trims at 3000)

## Data Flow

```
User Input → Build Prompt → HTTP POST to llama-server:8080/completion
          → Parse Response → Execute XML Tools → Update History → Loop
```

## LLM Integration

All agents communicate with llama-cpp-server on port 8080:
- Request: JSON with prompt, n_predict (4096), temperature, stop tokens
- Response: JSON with "content" field
- Default model: qwen2.5-coder-14b-instruct-q4_k_m.gguf

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

Runtime: GTK3, WebKit2GTK, libcurl, llama-cpp-server
