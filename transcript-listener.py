#!/usr/bin/env python3
"""
Transcript file listener - watches transcript files and sends content to agents
OPTIMIZED: Keeps LLM server and agents running persistently for faster response
"""

import os
import sys
import time
import subprocess
import threading
import json
import signal
import fcntl
from pathlib import Path
from datetime import datetime

# ============== SINGLE INSTANCE LOCK ==============
LOCK_FILE = "/tmp/transcript-listener.lock"

def acquire_lock():
    """Ensure only one instance runs at a time"""
    try:
        lock_fd = open(LOCK_FILE, 'w')
        fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        lock_fd.write(str(os.getpid()))
        lock_fd.flush()
        return lock_fd
    except IOError:
        print("[!] Another instance is already running. Exiting.")
        sys.exit(1)

TRANSCRIPT_DIR = "/root/transcripts"
AGENT_OS_DIR = "/root/workspace/agent-os"
LOG_DIR = "/root/agent-logs"
MAIN_LOG = f"{LOG_DIR}/main-history.json"
CODING_LOG = f"{LOG_DIR}/coding-history.json"
MAX_LOG_ENTRIES = 50  # Keep last N entries for context

# LLM server configs
TEXT_MODEL = os.path.expanduser("~/workspace/models/qwen2.5-coder-14b-instruct-q4_k_m.gguf")
VL_MODEL = os.path.expanduser("~/workspace/models/Qwen3-VL-8B-Instruct-Q8_0.gguf")  # Vision model
TEXT_PORT = 9090
VL_PORT = 9091

# File to agent mapping (ordered: main -> coding -> capture)
FILE_AGENTS = [
    ("main.txt", {
        "name": "agent",
        "binary": f"{AGENT_OS_DIR}/agent",
        "log": MAIN_LOG,
    }),
    ("coding.txt", {
        "name": "code-agent",
        "binary": f"{AGENT_OS_DIR}/code-agent",
        "log": CODING_LOG,
    }),
    ("capture.txt", {
        "name": "capture",
        "handler": None,  # Uses VL model, handled specially
    }),
]

def load_log(log_path):
    """Load action log from file"""
    if os.path.exists(log_path):
        try:
            with open(log_path, 'r') as f:
                return json.load(f)
        except:
            pass
    return []

def save_log(log_path, entries):
    """Save action log to file"""
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    entries = entries[-MAX_LOG_ENTRIES:]
    with open(log_path, 'w') as f:
        json.dump(entries, f, indent=2)

def add_log_entry(log_path, request, response):
    """Add a new entry to the log"""
    entries = load_log(log_path)
    entries.append({
        "timestamp": datetime.now().isoformat(),
        "request": request,
        "response": response[:500] if response else ""
    })
    save_log(log_path, entries)

def get_context_from_log(log_path, max_entries=5):
    """Get recent context from log for the agent - single line format"""
    entries = load_log(log_path)
    if not entries:
        return ""
    recent = entries[-max_entries:]
    context_parts = []
    for entry in recent:
        req = entry['request'][:80].replace('\n', ' ')
        context_parts.append(f"[{entry['timestamp'][:10]}] {req}")
    return "Previous: " + " | ".join(context_parts)


class PersistentAgent:
    """Keeps an agent process running and accepts multiple inputs"""
    def __init__(self, name, binary, log_path=None):
        self.name = name
        self.binary = binary
        self.log_path = log_path
        self.proc = None
        self.output_buffer = []
        self.reader_thread = None
        self.lock = threading.Lock()
        self.fresh_start = True  # True when agent just started, needs context

    def start(self):
        """Start the agent process"""
        if self.proc and self.proc.poll() is None:
            return True

        print(f"[*] Starting persistent {self.name}...")
        self.fresh_start = True  # Mark as fresh start, needs context on first message
        self.proc = subprocess.Popen(
            [self.binary],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            start_new_session=True
        )

        # Start output reader thread
        self.reader_thread = threading.Thread(target=self._read_output, daemon=True)
        self.reader_thread.start()

        # Wait for startup banner
        time.sleep(0.5)
        return self.proc.poll() is None

    def _read_output(self):
        """Continuously read output from agent"""
        try:
            for line in self.proc.stdout:
                with self.lock:
                    self.output_buffer.append(line)
                print(f"[{self.name}] {line}", end="")
        except:
            pass

    def send(self, text):
        """Send input to the running agent"""
        if not self.proc or self.proc.poll() is not None:
            if not self.start():
                return None

        # Clear output buffer
        with self.lock:
            self.output_buffer = []

        # Check for special commands
        text_lower = text.lower().strip()

        # "reset" or "clear" - send clear command to agent and skip processing
        if text_lower in ["reset", "clear"] or text_lower.startswith("reset ") or text_lower.startswith("clear "):
            try:
                self.proc.stdin.write("clear\n")
                self.proc.stdin.flush()
                print(f"[*] Sent clear to {self.name}")
                time.sleep(0.5)
                with self.lock:
                    return "".join(self.output_buffer) or "[Context cleared]"
            except:
                pass
            return "[Context cleared]"

        # "resume" - inject history context, then process rest of message
        if text_lower.startswith("resume"):
            # Extract the actual command after "resume"
            remaining = text[6:].strip()  # Remove "resume" prefix
            if remaining:
                text = remaining
            if self.log_path:
                context = get_context_from_log(self.log_path)
                if context:
                    text = f"{context} | Now: {text}"
                    print(f"[*] Resumed with history context")
        # Otherwise on fresh start, do NOT inject context (clean slate)

        self.fresh_start = False

        try:
            self.proc.stdin.write(text + "\n")
            self.proc.stdin.flush()
            print(f"[>] Sent to {self.name}: {text[:50]}...")

            # Wait for response (look for completion indicators)
            # Agent prints output then waits for next input
            time.sleep(2)  # Initial wait

            # Wait until no new output for 3 seconds (agent is done)
            last_count = -1
            stable_count = 0
            while stable_count < 3:
                with self.lock:
                    current_count = len(self.output_buffer)
                if current_count == last_count:
                    stable_count += 1
                else:
                    stable_count = 0
                    last_count = current_count
                time.sleep(1)

            with self.lock:
                return "".join(self.output_buffer)

        except BrokenPipeError:
            print(f"[!] {self.name} pipe broken, will restart on next request")
            self.proc = None
            return None
        except Exception as e:
            print(f"[!] Error sending to {self.name}: {e}")
            return None

    def stop(self):
        """Stop the agent"""
        if self.proc and self.proc.poll() is None:
            # Send exit command
            try:
                self.proc.stdin.write("exit\n")
                self.proc.stdin.flush()
                self.proc.wait(timeout=5)
            except:
                self.proc.terminate()
        self.proc = None

    def is_running(self):
        return self.proc and self.proc.poll() is None


class AgentManager:
    def __init__(self):
        self.llm_server = None
        self.llm_suspended = False
        self.persistent_agents = {}  # name -> PersistentAgent

        # Create persistent agents
        for filename, config in FILE_AGENTS:
            if config.get("binary"):
                self.persistent_agents[config["name"]] = PersistentAgent(
                    config["name"],
                    config["binary"],
                    config.get("log")  # Pass log path for context on restart
                )

    def ensure_llm_server(self, port=TEXT_PORT, model=TEXT_MODEL):
        """Make sure LLM server is running on specified port with specified model"""
        import urllib.request

        # Check if already running on correct port
        try:
            urllib.request.urlopen(f"http://localhost:{port}/health", timeout=2)
            print(f"[*] LLM server already running on port {port}")
            return True
        except:
            pass

        # Kill any existing llama-server to free VRAM
        print("[*] Stopping any existing llama-server...")
        subprocess.run(["pkill", "-f", "llama-server"], stderr=subprocess.DEVNULL)
        time.sleep(2)

        print(f"[*] Starting LLM server on port {port}...")
        self.llm_server = subprocess.Popen(
            ["llama-server", "-m", model,
             "-ngl", "34", "-c", "8192", "--port", str(port)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            start_new_session=True
        )

        # Wait for server to start
        for _ in range(120):
            try:
                urllib.request.urlopen(f"http://localhost:{port}/health", timeout=2)
                print(f"[*] LLM server ready on port {port}")
                return True
            except:
                time.sleep(1)
        print("[!] LLM server failed to start")
        return False

    def suspend_llm(self):
        """Suspend (SIGSTOP) the LLM server to free GPU for VL model"""
        if self.llm_server and self.llm_server.poll() is None:
            print("[*] Suspending LLM server...")
            os.kill(self.llm_server.pid, signal.SIGSTOP)
            self.llm_suspended = True
            time.sleep(1)

    def resume_llm(self):
        """Resume (SIGCONT) the suspended LLM server"""
        if self.llm_server and self.llm_suspended:
            print("[*] Resuming LLM server...")
            os.kill(self.llm_server.pid, signal.SIGCONT)
            self.llm_suspended = False
            time.sleep(1)

    def send_to_agent(self, agent_name, text, log_path=None):
        """Send text to a persistent agent"""
        if not self.ensure_llm_server():
            print(f"[!] Cannot use {agent_name}: LLM server not available")
            return

        # Clean input - ensure single line
        text_clean = text.replace('\n', ' ').strip()

        # For code-agent: if PROJECT specified, include directory tree and key file contents
        if agent_name == "code-agent" and "[PROJECT:" in text_clean:
            import re
            match = re.search(r'\[PROJECT:\s*([^\]]+)\]', text_clean)
            if match:
                project_path = match.group(1).strip()
                files = []
                try:
                    # Get recursive file listing (max depth 3)
                    result = subprocess.run(
                        ["find", project_path, "-maxdepth", "3", "-type", "f",
                         "(", "-name", "*.js", "-o", "-name", "*.ts", "-o", "-name", "*.py",
                         "-o", "-name", "*.cpp", "-o", "-name", "*.md", "-o", "-name", "*.json", ")"],
                        capture_output=True, text=True, timeout=5
                    )
                    if result.stdout.strip():
                        files = result.stdout.strip().split('\n')[:30]  # Limit to 30 files
                        file_list = '\n'.join(files)
                        text_clean = f"{text_clean}\n\nFILES IN PROJECT:\n{file_list}"
                        print(f"[*] Injected {len(files)} file paths into prompt")

                    # Auto-read key files and inject content
                    key_files = ['CLAUDE.md', 'README.md', 'package.json']
                    for key_file in key_files:
                        for f in files:
                            if f.endswith(f'/{key_file}') or f.endswith(f'/{key_file}'.lower()):
                                try:
                                    with open(f, 'r') as fh:
                                        content = fh.read()[:2000]  # Limit to 2000 chars
                                    text_clean = f"{text_clean}\n\n=== {f} ===\n{content}"
                                    print(f"[*] Injected content from {key_file}")
                                    break
                                except:
                                    pass
                except Exception as e:
                    print(f"[!] Failed to list project files: {e}")

        # Get or create persistent agent
        agent = self.persistent_agents.get(agent_name)
        if not agent:
            print(f"[!] Unknown agent: {agent_name}")
            return

        # Send to agent (agent keeps its own context now)
        response = agent.send(text_clean)

        # Log the action
        if log_path and response:
            add_log_entry(log_path, text, response)
            print(f"[*] Logged action to {log_path}")

    def handle_capture(self, content):
        """Handle capture (VL model) - suspend text LLM, run VL, resume"""
        print("[*] Processing capture with VL model...")

        # Suspend text LLM to free VRAM
        self.suspend_llm()

        # Run capture-screenshot.py which handles VL
        try:
            result = subprocess.run(
                ["python3", "/root/capture-screenshot.py"],
                input=content,
                capture_output=True,
                text=True,
                timeout=120
            )
            print(f"[capture] {result.stdout}")
            if result.stderr:
                print(f"[capture-err] {result.stderr}")
        except Exception as e:
            print(f"[!] Capture failed: {e}")

        # Resume text LLM
        self.resume_llm()

    def send_to_claude(self, content, source_file):
        """Send content to Claude Code CLI with context persistence"""
        import re

        CLAUDE_CONTEXT_FILE = "/root/agent-logs/claude_context.log"
        MAX_CONTEXT_CHARS = 8000  # Keep context manageable

        # Check for reset command
        content_lower = content.lower().strip()
        if content_lower in ["reset", "reset context", "clear context", "clear", "forget"]:
            if os.path.exists(CLAUDE_CONTEXT_FILE):
                os.unlink(CLAUDE_CONTEXT_FILE)
            print("[*] Claude context cleared")
            return

        # Check for compact command
        if content_lower in ["compact", "compact context", "summarize context", "compress"]:
            if not os.path.exists(CLAUDE_CONTEXT_FILE):
                print("[*] No context to compact")
                return

            with open(CLAUDE_CONTEXT_FILE, "r") as f:
                context = f.read()

            if len(context) < 500:
                print("[*] Context already compact")
                return

            print("[*] Compacting context...")
            compact_prompt = f"""Summarize this conversation history into a compact form that preserves:
1. Key topics discussed
2. Important decisions made
3. Any ongoing tasks or projects mentioned
4. Technical details that might be referenced later

Keep it concise but retain enough detail to continue the conversation naturally.

=== CONVERSATION HISTORY ===
{context}
=== END HISTORY ===

Provide a compact summary:"""

            try:
                result = subprocess.run(
                    ["claude", "-p", compact_prompt, "--permission-mode", "acceptEdits"],
                    capture_output=True,
                    text=True,
                    timeout=120
                )
                summary = result.stdout.strip()

                # Save compacted context
                compacted = f"=== COMPACTED CONTEXT ===\n{summary}\n=== END COMPACTED ===\n\n"
                with open(CLAUDE_CONTEXT_FILE, "w") as f:
                    f.write(compacted)

                print(f"[*] Context compacted: {len(context)} -> {len(compacted)} chars")
                print(f"[claude] {summary[:200]}...")
            except Exception as e:
                print(f"[!] Compact failed: {e}")
            return

        # Extract project path if present
        project_dir = "/root/workspace"
        match = re.search(r'\[PROJECT:\s*([^\]]+)\]', content)
        if match:
            project_dir = match.group(1).strip()
            content = re.sub(r'\[PROJECT:\s*[^\]]+\]\s*', '', content)

        # Check for analyse/crawl command
        if content_lower.startswith(("analyse", "analyze", "crawl")):
            # Extract target path from command or use project_dir
            parts = content.split(maxsplit=1)
            if len(parts) > 1:
                target = parts[1].strip()
                # If it's a relative path, make it absolute
                if not target.startswith("/"):
                    target = os.path.join(project_dir, target)
            else:
                target = project_dir

            if not os.path.exists(target):
                print(f"[!] Path not found: {target}")
                return

            print(f"[*] Analyzing codebase: {target}")

            # Gather code files
            try:
                code_content = []
                extensions = (".py", ".ts", ".tsx", ".js", ".jsx", ".sol", ".go", ".rs", ".cpp", ".c", ".h", ".java", ".md", ".json", ".yaml", ".yml", ".toml")
                exclude_dirs = ("node_modules", ".git", "artifacts", "dist", "build", "__pycache__", ".next", "venv", "env")

                for root, dirs, files in os.walk(target):
                    # Skip excluded directories
                    dirs[:] = [d for d in dirs if d not in exclude_dirs]

                    for f in files:
                        if f.endswith(extensions):
                            filepath = os.path.join(root, f)
                            rel_path = os.path.relpath(filepath, target)
                            try:
                                with open(filepath, 'r', errors='ignore') as file:
                                    content_lines = file.readlines()[:100]  # First 100 lines
                                    code_content.append(f"\n=== {rel_path} ===\n{''.join(content_lines)}")
                            except:
                                pass

                            if len(code_content) > 50:  # Limit files
                                break
                    if len(code_content) > 50:
                        break

                if not code_content:
                    print("[!] No code files found")
                    return

                code_dump = ''.join(code_content)[:15000]  # Limit total size

                analysis_prompt = f"""You are analyzing a codebase at '{target}'. Based on the code files below, write a comprehensive project analysis that includes:

1. **Project Overview** - What is this project? What problem does it solve?
2. **Architecture** - What are the main components (backend, frontend, mobile, blockchain, etc.)?
3. **Tech Stack** - What languages, frameworks, and tools are used?
4. **Key Features** - What functionality does this project provide?
5. **Project Structure** - How is the code organized?
6. **Entry Points** - Where does the application start? Main files?

Be detailed and specific based on the actual code you see:

{code_dump}"""

                result = subprocess.run(
                    ["claude", "-p", analysis_prompt, "--permission-mode", "acceptEdits"],
                    capture_output=True,
                    text=True,
                    timeout=300,
                    cwd=target
                )

                analysis = result.stdout.strip()
                print(f"[claude] {analysis}")

                # Save analysis to file
                analysis_file = os.path.join(target, "PROJECT_ANALYSIS.md")
                with open(analysis_file, "w") as f:
                    f.write(f"# Project Analysis\n\n{analysis}\n")
                print(f"[*] Analysis saved to: {analysis_file}")

            except Exception as e:
                print(f"[!] Analysis failed: {e}")
            return

        print(f"[*] Sending to Claude Code: {content[:50]}...")

        # Load existing context
        context = ""
        if os.path.exists(CLAUDE_CONTEXT_FILE):
            with open(CLAUDE_CONTEXT_FILE, "r") as f:
                context = f.read()

        # Build prompt with context
        if context:
            full_prompt = f"""You have previous conversation context below. Use it to understand references to earlier topics.

=== PREVIOUS CONTEXT ===
{context}
=== END CONTEXT ===

Current request: {content}

Respond to the current request, using context from our previous conversation if relevant."""
        else:
            full_prompt = content

        # Run claude CLI in print mode
        try:
            result = subprocess.run(
                ["claude", "-p", full_prompt, "--permission-mode", "acceptEdits"],
                capture_output=True,
                text=True,
                timeout=300,  # 5 minute timeout
                cwd=project_dir
            )
            response = result.stdout.strip()
            print(f"[claude] {response}")
            if result.stderr:
                print(f"[claude-err] {result.stderr}")

            # Append this exchange to context log
            new_entry = f"USER: {content}\nCLAUDE: {response}\n\n"
            context += new_entry

            # Trim context if too long (keep most recent)
            if len(context) > MAX_CONTEXT_CHARS:
                # Keep the last MAX_CONTEXT_CHARS characters
                context = "...(earlier context trimmed)...\n\n" + context[-MAX_CONTEXT_CHARS:]

            # Save updated context
            os.makedirs(os.path.dirname(CLAUDE_CONTEXT_FILE), exist_ok=True)
            with open(CLAUDE_CONTEXT_FILE, "w") as f:
                f.write(context)

        except subprocess.TimeoutExpired:
            print("[!] Claude request timed out (5 min)")
        except Exception as e:
            print(f"[!] Claude failed: {e}")

    def shutdown_llm(self):
        """Shut down the LLM server"""
        subprocess.run(["pkill", "-f", "llama-server"], stderr=subprocess.DEVNULL)
        self.llm_server = None
        self.llm_suspended = False
        time.sleep(1)

    def shutdown(self):
        """Clean up all agents and LLM"""
        print("[*] Shutting down persistent agents...")
        for name, agent in self.persistent_agents.items():
            agent.stop()
        if self.llm_server and self.llm_server.poll() is None:
            self.llm_server.terminate()


class TranscriptWatcher:
    def __init__(self, agent_manager):
        self.agent_manager = agent_manager
        self.file_mtimes = {}
        self.file_contents = {}
        self.startup_time = time.time()  # Ignore files modified before this

    def check_file(self, filename):
        """Check if file has new content"""
        filepath = os.path.join(TRANSCRIPT_DIR, filename)

        if not os.path.exists(filepath):
            return None

        mtime = os.path.getmtime(filepath)

        # Ignore files modified before listener started
        if mtime < self.startup_time:
            self.file_mtimes[filename] = mtime  # Track it so we catch future changes
            return None

        # Check if file was modified since last check
        if filename in self.file_mtimes:
            if mtime <= self.file_mtimes[filename]:
                return None

        self.file_mtimes[filename] = mtime

        # Read content
        try:
            with open(filepath, 'r') as f:
                content = f.read().strip()
        except:
            return None

        # Check if content changed
        if filename in self.file_contents:
            if content == self.file_contents[filename]:
                return None

        self.file_contents[filename] = content
        return content

    def watch(self):
        """Main watch loop - processes files in order: main -> coding -> capture"""
        print(f"[*] Watching {TRANSCRIPT_DIR} for changes...")
        print(f"[*] Files (in order): {', '.join(f for f, _ in FILE_AGENTS)}")
        print(f"[*] Logs saved to: {LOG_DIR}")
        print(f"[*] OPTIMIZED MODE: LLM server and agents stay running")

        # Pre-start LLM server
        self.agent_manager.ensure_llm_server()

        while True:
            # Process files in order
            for filename, config in FILE_AGENTS:
                content = self.check_file(filename)

                if content:
                    print(f"\n[*] Detected change in {filename}")

                    # Check for [CLAUDE] flag - route to Claude Code instead of local LLM
                    use_claude = content.startswith("[CLAUDE]")
                    if use_claude:
                        content = content[8:].strip()  # Remove [CLAUDE] prefix
                        print("[*] Claude mode - routing to Claude Code CLI")
                        self.agent_manager.send_to_claude(content, filename)
                        continue

                    if filename == "capture.txt":
                        # Special handling for capture (VL model)
                        self.agent_manager.handle_capture(content)
                    elif config.get("binary"):
                        self.agent_manager.send_to_agent(
                            config["name"],
                            content,
                            config.get("log")
                        )
                    else:
                        print(f"[*] {filename}: No handler configured")
                        print(f"    Content: {content[:100]}...")

            time.sleep(1)


def main():
    # Ensure single instance
    lock_fd = acquire_lock()
    print(f"[*] Lock acquired (PID: {os.getpid()})")

    os.makedirs(TRANSCRIPT_DIR, exist_ok=True)
    os.makedirs(LOG_DIR, exist_ok=True)

    agent_manager = AgentManager()
    watcher = TranscriptWatcher(agent_manager)

    try:
        watcher.watch()
    except KeyboardInterrupt:
        print("\n[*] Shutting down...")
        agent_manager.shutdown()
    finally:
        # Release lock
        fcntl.flock(lock_fd, fcntl.LOCK_UN)
        lock_fd.close()


if __name__ == "__main__":
    main()
