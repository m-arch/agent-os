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

| Binary | Description |
|--------|-------------|
| `agent` | Main interactive assistant with file tools and GUI spawning |
| `agent-view` | WebKit2-based embedded browser for GUI display |
| `code-agent` | Specialized code review agent with accept/reject workflow |

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

## License

MIT

## Contributing

Contributions welcome! Please open an issue or submit a pull request.
