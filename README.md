# Agent OS

A C++ AI-powered operating system assistant that bridges local LLMs with OS operations. It provides interactive AI interaction with file manipulation, command execution, and GUI capabilities.

## Features

- **Interactive REPL** - Natural language interface to your operating system
- **File Operations** - Read, create, edit, and delete files via AI commands
- **Command Execution** - Run shell commands through the AI assistant
- **GUI Generation** - AI can create and display HTML-based interfaces
- **Web Browsing** - Built-in WebKit-based browser component
- **Code Review Agent** - Specialized assistant for code modifications with diff visualization
- **Local LLM** - Runs entirely on local models via llama.cpp (no cloud dependencies)

## Components

### C++ Binaries

| Binary | Description |
|--------|-------------|
| `agent` | Main interactive assistant with file tools and GUI spawning |
| `agent-view` | WebKit2-based embedded browser for GUI display |
| `code-agent` | Specialized code review agent with accept/reject workflow |

### Python Scripts

| Script | Description |
|--------|-------------|
| `transcript-listener.py` | Background daemon that watches transcript files and routes content to agents |
| `transcript-buttons.py` | GTK3-based overlay widget with voice recording, transcription, and agent control |

## Requirements

### Build Dependencies
- g++ (C++17)
- make
- pkg-config
- libcurl-dev
- libreadline-dev
- libgtk-3-dev
- libwebkit2gtk-4.0-dev

### Runtime Dependencies
- GTK3
- WebKit2GTK
- libcurl
- [llama.cpp](https://github.com/ggerganov/llama.cpp) server

## Installation

```bash
# Install build dependencies (Arch Linux)
sudo pacman -S base-devel curl gtk3 webkit2gtk

# Install build dependencies (Ubuntu/Debian)
sudo apt install build-essential libcurl4-openssl-dev libreadline-dev libgtk-3-dev libwebkit2gtk-4.0-dev

# Clone and build
git clone https://github.com/m-arch/agent-os.git
cd agent-os
make all

# Optional: Install to /usr/local/bin
sudo make install
```

## Usage

### Quick Start

```bash
# Start LLM server and agent together
./start-agent.sh

# Or run agent directly (requires llama-server on port 9090)
./agent
```

### Commands

Once running, you can interact naturally:

```
> Read my .bashrc file
> Create a new Python script that prints hello world
> Run ls -la in my home directory
> Show me a calculator app
> Open google.com
> Delete the temp folder
```

### Tool Syntax

The AI uses XML-based tools internally:

| Tool | Syntax | Description |
|------|--------|-------------|
| Read | `<read path="..."/>` | Read file contents |
| Create | `<create path="...">content</create>` | Create new files |
| Edit | `<edit path="..."><old>...</old><new>...</new></edit>` | Modify files |
| Run | `<run>command</run>` | Execute shell commands |
| GUI | `<gui>html</gui>` | Display HTML interface |
| URL | `<url>https://...</url>` | Open URL in browser |
| Delete | `<delete path="..."/>` | Delete with confirmation |

## Configuration

Default settings in `agent.cpp`:
- **LLM URL**: `http://localhost:9090/completion`
- **Max Tokens**: 4096
- **Temperature**: 0.7
- **Context Window**: 8000 chars (trims at 6000)

### Recommended Models

Compatible with Qwen-based models via llama.cpp:
- `qwen2.5-coder-14b-instruct-q4_k_m.gguf` - Balanced performance
- `Qwen3-Coder-30B-A3B-Instruct-Q3_K_M.gguf` - Best quality

## Architecture

```
User Input → Build Prompt → HTTP POST to llama-server:9090
          → Parse Response → Execute XML Tools → Update History → Loop
```

## Python Scripts Detail

### transcript-listener.py

A background daemon that monitors transcript files and routes content to the appropriate AI agents.

**Key Features:**
- **Single Instance Lock** - Uses `/tmp/transcript-listener.lock` to ensure only one listener runs
- **File Watching** - Monitors `/root/transcripts/` for changes in `main.txt`, `coding.txt`, and `capture.txt`
- **Persistent Agents** - Keeps agent processes running for faster response times
- **LLM Server Management** - Auto-starts llama-server, manages VRAM by suspending/resuming for vision tasks
- **Context Logging** - Maintains JSON action logs (last 50 entries) for conversation context
- **Claude Code Integration** - Routes `[CLAUDE]` prefixed messages to Claude Code CLI
- **Codebase Analysis** - `analyse` command triggers full project analysis with Claude

**Agent Routing:**
| File | Agent | Purpose |
|------|-------|---------|
| `main.txt` | `agent` | General assistant tasks |
| `coding.txt` | `code-agent` | Code review and modifications |
| `capture.txt` | VL model | Screenshot + voice analysis |

**Special Commands:**
- `reset` / `clear` - Clear agent context
- `resume <text>` - Continue with injected history context
- `[CLAUDE] <text>` - Route to Claude Code instead of local LLM
- `analyse <path>` - Generate PROJECT_ANALYSIS.md for codebase

### transcript-buttons.py

A GTK3 Layer Shell overlay widget providing a voice-controlled interface to the agent system.

**Key Features:**
- **Layer Shell Overlay** - Always-on-top widget anchored to screen edge
- **Voice Recording** - Records audio via Razer Kiyo or default capture device
- **Whisper Transcription** - Uses whisper.cpp with GPU acceleration (large-v3 model)
- **System Monitor** - Real-time CPU, RAM, and GPU/VRAM usage display
- **Output Log** - Scrollable view of agent responses
- **Text Input** - Type messages directly to agents
- **Claude Toggle** - Switch between local LLM and Claude Code

**Buttons:**
| Button | Function |
|--------|----------|
| Main (Red) | Record voice → transcribe → send to main agent |
| Coding (Green) | Select project folder → record → send to code-agent |
| Capture (Blue) | Screenshot selection → record voice → VL model analysis |
| Clau$e (Gray/Purple) | Toggle Claude Code mode on/off |
| ⟳ | Restart entire system (kills processes, respawns widget) |

**Capture Workflow:**
1. Click Capture → select screen region with slurp
2. Screenshot saved, audio recording starts
3. Click again to stop recording
4. Whisper transcribes audio
5. VL model or Claude analyzes screenshot + transcript
6. Response routed to appropriate agent

**Dependencies:**
- GTK3 + GtkLayerShell (Wayland overlay support)
- whisper.cpp with CUDA support
- ffmpeg (audio conversion)
- slurp + grim (Wayland screenshot tools)
- nvidia-smi (GPU monitoring)

## License

MIT

## Contributing

Contributions welcome! Please open an issue or submit a pull request.
