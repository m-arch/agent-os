# Agent OS - Comprehensive System Analysis

## Executive Summary

Agent OS is a sophisticated C++ AI-powered operating system assistant that bridges local LLMs with OS operations. It provides a multi-component ecosystem enabling voice control, interactive AI assistance, code review, and GUI generation. The system is entirely self-contained with no cloud dependencies.

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                    SYSTEM ARCHITECTURE                      │
└─────────────────────────────────────────────────────────────┘

Voice/Text Input
    ↓
transcript-buttons.py (GTK3 Layer Shell Widget)
    ↓ (writes to /root/transcripts/)
transcript-listener.py (File Watcher Daemon)
    ↓ (routes to)
Three Agent Types:
    ├→ agent (General Assistant)
    ├→ code-agent (Code Review Specialist)
    └→ VL Model (Vision Language)
    ↓
llama-server (Port 9090/9091)
    ↓
LLM Response → XML Tool Parsing
    ↓
Output to transcript-buttons.py UI
```

---

## Project Structure

```
/root/workspace/agent-os/
├── Core C++ Binaries:
│   ├── agent.cpp        (18.3 KB) - Main interactive assistant
│   ├── agent-view.cpp   (9.3 KB)  - WebKit2 browser component
│   └── code-agent.cpp   (27.6 KB) - Code review specialist
│
├── Python Scripts:
│   ├── transcript-listener.py (28.8 KB) - File watcher daemon
│   └── transcript-buttons.py  (40.4 KB) - GTK3 overlay widget
│
├── Build System:
│   ├── makefile         - G++17 compiler with multilib linking
│   └── start-agent.sh   - LLM server startup script
│
├── Documentation:
│   ├── README.md        - User documentation
│   └── CLAUDE.md        - Developer guidance
│
└── Compiled Binaries:
    ├── agent            (79 KB)
    ├── agent-view       (35.9 KB)
    └── code-agent       (93.5 KB)
```

**Total Codebase Size:** ~3,930 lines of core logic
- C++: ~2,100 lines
- Python: ~1,830 lines

---

## Component Analysis

### 1. Agent (agent.cpp) - Main Interactive Assistant

**Purpose:** Primary REPL interface for user interaction with LLM and system integration

**Configuration:**
| Parameter | Value |
|-----------|-------|
| LLM Endpoint | http://localhost:9090/completion |
| Max Tokens | 4096 |
| Temperature | 0.7 |
| Context Window | 8000 chars (trims at 6000) |

**XML Tool System:**
| Tag | Syntax | Behavior |
|-----|--------|----------|
| read | `<read path="/path"/>` | Read file contents |
| create | `<create path="/path">content</create>` | Create new file |
| edit | `<edit path="/path"><old>...</old><new>...</new></edit>` | Find & replace |
| run | `<run>command</run>` | Execute shell command |
| gui | `<gui>html</gui>` | Display HTML in agent-view |
| url | `<url>https://...</url>` | Open URL in browser |
| delete | `<delete path="/path"/>` | Delete with confirmation |

**Key Functions:**
- `query_llm()` - HTTP communication with LLM server
- `process_tools()` - XML parsing and execution dispatcher
- `show_gui()` - Process forking with HTML file creation

---

### 2. Agent-View (agent-view.cpp) - WebKit2 Browser

**Purpose:** Lightweight embedded browser for HTML interfaces and GUI display

**Features:**
- Cookie persistence in `~/.agent_browser/`
- History logging to `~/.agent_history`
- JavaScript console capture
- Navigation toolbar (back/forward)
- Hardware acceleration disabled for stability

**Technical Details:**
- Fixed window size: 900x700
- Full JavaScript execution enabled
- Intercepts window.open() for same-window loading

---

### 3. Code-Agent (code-agent.cpp) - Code Review Specialist

**Purpose:** Interactive code modification with diff visualization and safety constraints

**Configuration:**
| Parameter | Value |
|-----------|-------|
| Temperature | 0.3 (lower for precision) |
| Max Context | 4000 chars (trims at 3000) |
| Workspace Boundary | /root/workspace/ |

**Safety Features:**
- Workspace boundary enforcement
- Blocked commands: `rm -rf`, `sudo`, `chmod`, `chown`, `dd`, `> /`
- File size limits: 4000 bytes per file
- Path validation with realpath()

**Change Block Format:**
```xml
<change file="path">
  <description>What this change does</description>
  <old>text to replace</old>
  <new>replacement text</new>
</change>
```

**Commands:**
- `/file <path>` - Load file into context
- `/analyze <path>` - Start codebase analysis mode
- `/clear` - Clear context
- `/exit` - Quit

---

### 4. Transcript-Listener (transcript-listener.py) - Background Daemon

**Purpose:** Multi-agent orchestrator with LLM server management and file watching

**Core Classes:**

#### PersistentAgent
- Manages long-running agent subprocesses
- Stdin/stdout communication with stability detection
- 3-second wait for output completion

#### AgentManager
- LLM server health checks and startup
- VRAM management via SIGSTOP/SIGCONT
- Project context injection for coding tasks
- Claude Code CLI integration

#### TranscriptWatcher
- Monitors `/root/transcripts/` directory
- Routes by filename:
  - `main.txt` → agent
  - `coding.txt` → code-agent
  - `capture.txt` → VL model
- 1-second polling interval

**Configuration:**
```python
TRANSCRIPT_DIR = "/root/transcripts"
TEXT_MODEL = "~/workspace/models/qwen2.5-coder-7b-instruct-q4_k_m.gguf"
VL_MODEL = "~/workspace/models/Qwen3-VL-8B-Instruct-Q8_0.gguf"
TEXT_PORT = 9090
VL_PORT = 9091
```

---

### 5. Transcript-Buttons (transcript-buttons.py) - Voice Control UI

**Purpose:** Always-on-top GTK3 Layer Shell widget for voice recording and agent control

**UI Components:**
| Button | Color | Function |
|--------|-------|----------|
| Main | Red | General agent recording |
| Coding | Green | Code agent with folder selection |
| Capture | Blue | Screenshot + voice capture |
| Claude | Purple/Gray | Toggle Claude mode |
| Restart | ⟳ | System restart |

**Recording Flow:**
1. Click button → Start arecord
2. Click again → Stop recording
3. Convert audio with ffmpeg (16kHz mono)
4. Transcribe with whisper-cli (GPU)
5. Write to transcript file
6. Listener routes to agent

**System Monitor:**
- CPU, RAM, GPU, VRAM usage
- Updated every 2 seconds
- Reads from /proc/stat, /proc/meminfo, nvidia-smi

---

## Data Flow

```
┌──────────────────────────────────────────────────────────────┐
│                    COMPLETE DATA FLOW                        │
└──────────────────────────────────────────────────────────────┘

USER INPUT SOURCES:
  ├─ Voice (transcript-buttons.py):
  │   ├─ Main: arecord → whisper → main.txt
  │   ├─ Coding: arecord → whisper → coding.txt
  │   └─ Capture: screenshot + audio → VL model
  │
  ├─ Text Input (transcript-buttons.py):
  │   └─ Entry widget → main.txt
  │
  └─ GUI Feedback (agent.cpp):
      └─ /tmp/agent-fifo → agent REPL

PROCESSING:
  transcript-listener.py watches files
    ↓
  Routes to appropriate agent
    ↓
  Agent queries llama-server
    ↓
  Parses XML tools from response
    ↓
  Executes system operations
    ↓
  Returns output to UI
```

---

## Inter-Process Communication

| Mechanism | Purpose | Components |
|-----------|---------|------------|
| Files | Primary IPC | transcripts/, agent-logs/ |
| Pipes (stdio) | Agent communication | PersistentAgent subprocess |
| HTTP | LLM queries | agent → llama-server |
| Signals | VRAM management | SIGSTOP/SIGCONT to llama-server |
| Named Pipes | GUI feedback | /tmp/agent-fifo |
| GTK Events | UI updates | GLib.idle_add() |

---

## LLM Integration

**Request Format:**
```json
{
  "prompt": "...",
  "n_predict": 4096,
  "temperature": 0.7,
  "stop": ["</s>", "User:", "<|im_end|>", "<|endoftext|>"]
}
```

**Endpoint:** `http://localhost:9090/completion`

**Supported Models:**
- qwen2.5-coder-7b-instruct (4.7GB)
- qwen2.5-coder-14b-instruct (9GB)
- Qwen3-Coder-30B (14.7GB)
- Qwen3-VL-8B (8.7GB) - Vision

---

## Security Considerations

### Identified Risks

1. **Command Injection**
   - `popen()` without shell escaping in agent.cpp
   - User input flows directly to system()

2. **Path Traversal**
   - agent.cpp has no path validation
   - Symlinks could bypass workspace boundary

3. **XML Injection**
   - Simple string-based parsing
   - No DTD/schema validation

4. **Credential Exposure**
   - Action logs in plaintext JSON
   - May contain API keys

### Existing Mitigations

- code-agent has workspace boundary enforcement
- Dangerous command patterns blocked
- Delete operations require zenity confirmation

---

## Performance Considerations

### Optimizations Present

- Persistent agent processes (no startup overhead)
- VRAM suspension for vision tasks
- Aggressive context trimming
- Non-blocking file I/O with threading

### Areas for Improvement

1. **File Watching**
   - 1-second polling could miss rapid changes
   - Consider inotify integration

2. **Context Management**
   - Hard-coded limits (4000-8000 chars)
   - No intelligent compression

3. **Error Recovery**
   - No retry logic for failed LLM calls
   - Audio transcription failures silent

4. **Concurrency**
   - Race conditions possible with rapid button clicks
   - Shared state access needs review

---

## Dependencies

### Build Requirements
- GCC/G++ with C++17 support
- libcurl-dev
- libreadline-dev
- libgtk-3-dev
- libwebkit2gtk-4.0-dev
- pkg-config

### Runtime Requirements
- llama.cpp (with CUDA/Metal)
- whisper.cpp (GPU support)
- Python 3.7+
- GTK3 runtime
- ffmpeg, arecord
- grim, slurp (Wayland)
- zenity, nvidia-smi

---

## Workflow Examples

### Voice Command to File Creation

```
User speaks: "Create a Python hello world script"
    ↓
transcript-buttons.py records audio
    ↓
Whisper transcribes to /root/transcripts/main.txt
    ↓
transcript-listener.py detects change
    ↓
Routes to agent binary
    ↓
agent.cpp queries LLM
    ↓
LLM responds: <create path="/tmp/hello.py">print('hello')</create>
    ↓
agent.cpp creates file
    ↓
Output appears in UI
```

### Code Review with Context

```
User clicks Coding button, selects project folder
    ↓
Records: "Review the API handler"
    ↓
[PROJECT: /root/workspace/my-project] prepended
    ↓
code-agent receives with file context injected
    ↓
Returns structured <change> blocks
    ↓
Changes auto-applied with diff display
```

---

## Recommendations

### High Priority

1. **Add input sanitization** for shell commands
2. **Implement path validation** in agent.cpp
3. **Add retry logic** for LLM communication failures
4. **Use inotify** instead of polling for file watching

### Medium Priority

5. **Improve context management** with smarter trimming
6. **Add visual feedback** for recording state
7. **Implement graceful degradation** on component failures
8. **Add log encryption** or rotation

### Low Priority

9. **Model preloading** for faster VL switching
10. **Clipboard integration** for code sharing
11. **Multi-language whisper support**
12. **Web-based configuration interface**

---

## Conclusion

Agent OS represents a well-architected local AI assistant with sophisticated multi-modal input handling and deep system integration. The polyglot approach (C++ for performance-critical components, Python for orchestration) is appropriate for the use case.

Key strengths:
- No cloud dependencies
- GPU acceleration throughout
- Persistent processes for fast response
- Specialized agents for different tasks

Areas needing attention:
- Security hardening for shell execution
- Error recovery and retry logic
- Context management optimization

The system is production-ready for personal development use but would benefit from the recommended hardening before broader deployment.

---

## Appendix: Context Drift Bug Analysis & Fix

### Issue Summary

When using the coding agent with a `[PROJECT: /root/workspace/agent-os]` directive, the agent was drifting outside the specified project directory and listing files from the parent `/root/workspace` directory instead of staying focused on the project.

#### Example of the Problem
```
Request: "Do a discovery run for Agent OS and create an analysis.md file"
[PROJECT: /root/workspace/agent-os]

Agent Response:
>>> [Thinking...]
[Listing: /root/workspace]  <-- WRONG! Should be /root/workspace/agent-os
total 32
drwxr-xr-x  7 root root 4096 Dec  5 08:39 .
drwx------ 18 root root 4096 Dec  5 20:45 ..
drwxr-xr-x  4 root root 4096 Dec  5 20:36 agent-os
...
```

### Root Causes Identified

1. **No Project Boundary Instruction in Prompt Injection** (`transcript-listener.py:381-426`)
   - The `[PROJECT: path]` tag was being parsed and files were injected, but there was NO explicit instruction telling the LLM to stay within that directory boundary.

2. **Default Workspace Too Broad** (`code-agent.cpp:128`)
   - `const std::string WORKSPACE = "/root/workspace"` allowed access to sibling projects.

3. **No Dynamic Project Context Tracking**
   - No mechanism to track which specific project was being worked on during a session.

4. **System Prompt Lacked Project Focus Instructions** (`code-agent.cpp:379-405`)

### Fixes Implemented

1. **Project Boundary Injection** in `transcript-listener.py:400-410`
   ```python
   project_constraint = f"""
   === PROJECT CONTEXT ===
   CURRENT PROJECT: {project_path}
   IMPORTANT: You are working ONLY on this project.
   === END PROJECT CONTEXT ===
   """
   ```

2. **Dynamic Project Directory Tracking** in `code-agent.cpp`
   - Added `active_project_dir` global variable
   - Added `check_project_context()` function
   - Added `get_effective_workspace()` function

3. **Updated System Prompt** with project focus rules

4. **New `/project <path>` Command** for manual project management

5. **Clear Resets Project Context** - `/clear` now resets `active_project_dir`

### Files Modified

- `transcript-listener.py` - Added project boundary injection
- `code-agent.cpp` - Added dynamic project tracking and commands

### To Apply Changes

```bash
cd /root/workspace/agent-os
make code-agent
```

---

*Generated: December 5, 2025*
*Agent OS Version: Current master branch*
