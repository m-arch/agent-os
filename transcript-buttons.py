#!/usr/bin/env python3

import gi
gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gtk, Gdk, GLib, GtkLayerShell, Pango
import subprocess
import os
import signal
import threading

WHISPER_CLI = "/root/workspace/whisper.cpp/build/bin/whisper-cli"
MODEL = "/root/workspace/whisper.cpp/models/ggml-small.bin"
TRANSCRIPT_DIR = "/root/transcripts"

def get_audio_device():
    """Find Razer Kiyo or fallback to first capture device"""
    try:
        result = subprocess.run(["arecord", "-l"], capture_output=True, text=True)
        for line in result.stdout.split('\n'):
            if 'Kiyo' in line:
                # Extract card number from "card X:"
                card = line.split(':')[0].replace('card ', '')
                return f"plughw:{card},0"
        # Fallback to default
        return "plughw:0,0"
    except:
        return "plughw:0,0"

AUDIO_DEVICE = get_audio_device()

class StatusWindow(Gtk.Window):
    def __init__(self):
        super().__init__()

        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.OVERLAY)
        GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.BOTTOM, True)
        GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.RIGHT, True)
        GtkLayerShell.set_margin(self, GtkLayerShell.Edge.BOTTOM, 20)
        GtkLayerShell.set_margin(self, GtkLayerShell.Edge.RIGHT, 20)

        self.label = Gtk.Label(label="Processing...")

        css = Gtk.CssProvider()
        css.load_from_data(b'''
            window {
                background: #333333;
            }
            label {
                color: #ffaa00;
                padding: 8px 12px;
            }
        ''')
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(),
            css,
            Gtk.STYLE_PROVIDER_PRIORITY_USER
        )

        self.add(self.label)

class TranscriptButtons(Gtk.Window):
    def __init__(self):
        super().__init__()

        # Load CSS FIRST before any widgets are created
        self._setup_css()

        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.OVERLAY)
        GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.TOP, True)
        GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.BOTTOM, True)
        GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.RIGHT, True)
        GtkLayerShell.set_margin(self, GtkLayerShell.Edge.TOP, 10)
        GtkLayerShell.set_margin(self, GtkLayerShell.Edge.BOTTOM, 10)
        GtkLayerShell.set_margin(self, GtkLayerShell.Edge.RIGHT, 10)
        GtkLayerShell.set_keyboard_mode(self, GtkLayerShell.KeyboardMode.ON_DEMAND)

        # Setup UI after CSS and layer shell are ready
        self._setup_ui()

    def _setup_css(self):
        """Load CSS before widgets are created"""
        self._global_css = Gtk.CssProvider()
        self._global_css.load_from_data(b'''
            textview, textview text {
                background-color: #1a1a1a;
                color: #00ff00;
                font-family: monospace;
                font-size: 9pt;
            }
            scrolledwindow {
                background-color: #1a1a1a;
            }
            scrolledwindow undershoot.top,
            scrolledwindow undershoot.bottom,
            scrolledwindow overshoot.top,
            scrolledwindow overshoot.bottom {
                background: none;
            }
            entry {
                background-color: #2a2a2a;
                color: #ffffff;
                border: 1px solid #444444;
                min-height: 24px;
            }
            window {
                background-color: #333333;
            }
            label {
                font-size: 9pt;
            }
        ''')
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(),
            self._global_css,
            Gtk.STYLE_PROVIDER_PRIORITY_USER + 100
        )

    def _setup_ui(self):
        """Setup all UI widgets after CSS is loaded"""
        os.makedirs(TRANSCRIPT_DIR, exist_ok=True)

        # State tracking
        self.recording_process = None
        self.current_button = None
        self.current_name = None
        self.current_file = None
        self.temp_wav = None
        self.processing_count = 0
        self.status_window = None

        # Capture state
        self.capture_recording = None
        self.capture_button = None
        self.capture_screenshot_path = None
        self.capture_audio_path = None

        # Claude mode toggle
        self.use_claude = False

        # Button colors (unified agent: no separate coding button)
        self.colors = {
            "main": "#ff0000",
            "capture": "#0000ff",
            "recording": "#ffff00",
            "claude_on": "#9933ff",   # Purple when Claude enabled
            "claude_off": "#666666",  # Gray when Claude disabled
        }

        # Main container with fixed width
        main_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=2)
        main_box.set_size_request(390, -1)  # Fixed width to prevent resizing
        main_box.set_margin_start(4)
        main_box.set_margin_end(4)
        main_box.set_margin_top(4)
        main_box.set_margin_bottom(4)

        # Buttons row - Unified agent: Main handles both OS tasks and coding
        # User can say "project /path" via voice to set coding context
        btn_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)

        self.btn_main = Gtk.Button(label="Command")
        self.btn_capture = Gtk.Button(label="Capture")

        self.buttons = {
            "main": (self.btn_main, "main.txt", self.colors["main"]),
            "capture": (self.btn_capture, "capture.txt", self.colors["capture"])
        }

        for name, (btn, filename, color) in self.buttons.items():
            self.set_button_color(btn, color)
            btn.connect("clicked", self.on_button_clicked, name, filename)
            btn_box.pack_start(btn, True, True, 0)

        # Claude toggle button
        self.btn_claude = Gtk.Button(label="Clau$e")
        self.set_button_color(self.btn_claude, self.colors["claude_off"])
        self.btn_claude.connect("clicked", self.on_claude_toggle)
        btn_box.pack_start(self.btn_claude, True, True, 0)

        # Restart button (small with refresh icon)
        self.btn_restart = Gtk.Button(label="⟳")
        self.btn_restart.set_size_request(30, -1)  # Small width
        self.set_button_color(self.btn_restart, "#444444")
        self.btn_restart.connect("clicked", self.on_restart)
        btn_box.pack_start(self.btn_restart, False, False, 0)

        main_box.pack_start(btn_box, False, False, 0)

        # System monitor label
        self.sys_label = Gtk.Label()
        self.sys_label.set_markup('<span font="monospace 8" foreground="#aaaaaa">...</span>')
        self.sys_label.set_xalign(0)
        main_box.pack_start(self.sys_label, False, False, 0)

        # Output text area (scrollable)
        self.output_buffer = Gtk.TextBuffer()
        self.output_view = Gtk.TextView(buffer=self.output_buffer)
        self.output_view.set_editable(False)
        self.output_view.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        self.output_view.set_cursor_visible(False)
        self.output_view.set_monospace(True)
        # Set font directly to ensure it doesn't change
        font_desc = Pango.FontDescription("monospace 9")
        self.output_view.override_font(font_desc)

        self.output_scroll = Gtk.ScrolledWindow()
        self.output_scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        self.output_scroll.set_size_request(380, -1)  # Fixed width
        self.output_scroll.add(self.output_view)
        main_box.pack_start(self.output_scroll, True, True, 0)  # expand=True to fill vertical space

        # Output line buffer for smooth updates
        self.output_lines = []
        self.max_output_lines = 500  # Keep last 500 lines
        self.auto_scroll = True  # Track if we should auto-scroll

        # Detect when user scrolls away from bottom
        vadj = self.output_scroll.get_vadjustment()
        vadj.connect("value-changed", self.on_scroll_changed)

        # Text input row
        input_box = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=4)

        self.text_input = Gtk.Entry()
        self.text_input.set_placeholder_text("Type message to agent...")
        self.text_input.connect("activate", self.on_send_message)  # Enter key
        self.text_input.connect("button-press-event", self.on_input_clicked)  # Focus on click
        self.text_input.set_can_focus(True)
        input_box.pack_start(self.text_input, True, True, 0)

        self.btn_send = Gtk.Button(label="▶")
        self.btn_send.set_size_request(40, -1)
        self.set_button_color(self.btn_send, "#00aa00")
        self.btn_send.connect("clicked", self.on_send_message)
        input_box.pack_start(self.btn_send, False, False, 0)

        main_box.pack_start(input_box, False, False, 0)

        self.add(main_box)

        # Start system monitor update
        GLib.timeout_add(2000, self.update_system_stats)

        # Start transcript listener as subprocess
        self.start_listener()

    def on_claude_toggle(self, button):
        """Toggle between local LLM and Claude Code"""
        self.use_claude = not self.use_claude
        if self.use_claude:
            self.set_button_color(self.btn_claude, self.colors["claude_on"])
            self.btn_claude.set_label("CLAUDE")
            self.append_output("[*] Claude mode ENABLED")
        else:
            self.set_button_color(self.btn_claude, self.colors["claude_off"])
            self.btn_claude.set_label("Clau$e")
            self.append_output("[*] Claude mode DISABLED")

    def on_input_clicked(self, widget, event):
        """Grab focus when input field is clicked"""
        self.text_input.grab_focus()
        return False  # Let event propagate

    def on_send_message(self, widget):
        """Send typed message to agent"""
        text = self.text_input.get_text().strip()
        if not text:
            return

        # Clear input
        self.text_input.set_text("")

        # Determine which transcript file to use
        if self.use_claude:
            # Claude mode - write to main.txt with [CLAUDE] prefix
            transcript_file = os.path.join(TRANSCRIPT_DIR, "main.txt")
            content = f"[CLAUDE] {text}"
        else:
            # Local LLM - write to main.txt
            transcript_file = os.path.join(TRANSCRIPT_DIR, "main.txt")
            content = text

        # Write to transcript file (listener will pick it up)
        with open(transcript_file, 'w') as f:
            f.write(content)

        self.append_output(f"[>] Sent: {text[:50]}...")

    def on_restart(self, button):
        """Restart everything including this widget"""
        self.append_output("[*] Restarting everything...")

        # Kill existing listener
        if hasattr(self, 'listener_process') and self.listener_process:
            try:
                self.listener_process.terminate()
                self.listener_process.wait(timeout=2)
            except:
                pass

        # Kill any orphaned processes
        subprocess.run(["pkill", "-9", "-f", "transcript-listener"], stderr=subprocess.DEVNULL)
        subprocess.run(["pkill", "-9", "-f", "llama-server"], stderr=subprocess.DEVNULL)
        subprocess.run(["pkill", "-9", "-f", "arecord"], stderr=subprocess.DEVNULL)
        subprocess.run(["pkill", "-9", "-f", "whisper-cli"], stderr=subprocess.DEVNULL)
        subprocess.run(["rm", "-f", "/tmp/transcript-listener.lock"], stderr=subprocess.DEVNULL)

        # Spawn new instance of buttons widget, then exit
        subprocess.Popen(
            ["python3", "/root/workspace/agent-os/transcript-buttons.py"],
            start_new_session=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL
        )

        # Exit this instance
        GLib.timeout_add(100, Gtk.main_quit)

    def start_listener(self):
        """Start transcript-listener.py as subprocess and capture output"""
        import io

        self.listener_process = None

        # Kill any existing listener
        subprocess.run(["pkill", "-f", "transcript-listener"], stderr=subprocess.DEVNULL)
        subprocess.run(["rm", "-f", "/tmp/transcript-listener.lock"], stderr=subprocess.DEVNULL)

        # Start listener with unbuffered output
        env = os.environ.copy()
        env["PYTHONUNBUFFERED"] = "1"

        self.listener_process = subprocess.Popen(
            ["python3", "-u", "/root/workspace/agent-os/transcript-listener.py"],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            env=env,
            start_new_session=True
        )

        # Start thread to read listener output
        self.listener_thread = threading.Thread(target=self.read_listener_output, daemon=True)
        self.listener_thread.start()

        self.append_output("[*] Listener started")

    def read_listener_output(self):
        """Read output from listener process and append to text view"""
        try:
            for line in iter(self.listener_process.stdout.readline, b''):
                if line:
                    text = line.decode('utf-8', errors='replace').rstrip()
                    GLib.idle_add(self.append_output, text)
        except Exception as e:
            GLib.idle_add(self.append_output, f"[!] Listener error: {e}")

    def on_scroll_changed(self, adj):
        """Track if user is at bottom of scroll"""
        # Don't update during programmatic scrolling
        if getattr(self, '_scrolling_programmatically', False):
            return
        # Check if scrolled to bottom (with small tolerance)
        at_bottom = adj.get_value() >= adj.get_upper() - adj.get_page_size() - 20
        self.auto_scroll = at_bottom

    def scroll_to_bottom(self):
        """Scroll to bottom of output"""
        self._scrolling_programmatically = True
        adj = self.output_scroll.get_vadjustment()
        adj.set_value(adj.get_upper() - adj.get_page_size())
        self._scrolling_programmatically = False
        return False  # For GLib.idle_add

    def append_output(self, text):
        """Append text to output buffer (thread-safe via GLib.idle_add)"""
        # Strip ANSI codes for cleaner display
        import re
        text = re.sub(r'\x1b\[[0-9;]*m', '', text)

        # Skip empty lines and repetitive/spam messages
        text_stripped = text.strip()
        if not text_stripped:
            return False

        # Skip repetitive messages
        skip_patterns = [">>>", "[Thinking...]", ">>> [Thinking...]", ">>> >>> [Thinking...]",
                        "Unknown command", ">>> Unknown command"]
        if text_stripped in skip_patterns:
            # Only show once if last line was similar
            if self.output_lines:
                last = self.output_lines[-1].strip()
                if any(p in last for p in skip_patterns):
                    return False

        # Check if at bottom BEFORE adding text
        adj = self.output_scroll.get_vadjustment()
        was_at_bottom = adj.get_value() >= adj.get_upper() - adj.get_page_size() - 20

        self.output_lines.append(text)
        if len(self.output_lines) > self.max_output_lines:
            self.output_lines = self.output_lines[-self.max_output_lines:]

        self.output_buffer.set_text('\n'.join(self.output_lines))

        # Only auto-scroll if user WAS at bottom before new text
        if was_at_bottom:
            GLib.idle_add(self.scroll_to_bottom)

        return False  # For GLib.idle_add

    def log(self, text):
        """Thread-safe logging to output area"""
        GLib.idle_add(self.append_output, text)

    def update_system_stats(self):
        """Update system stats display"""
        try:
            # CPU usage
            with open('/proc/stat', 'r') as f:
                cpu_line = f.readline()
            cpu_parts = cpu_line.split()[1:5]
            cpu_total = sum(int(x) for x in cpu_parts)
            cpu_idle = int(cpu_parts[3])

            if hasattr(self, '_last_cpu'):
                total_diff = cpu_total - self._last_cpu[0]
                idle_diff = cpu_idle - self._last_cpu[1]
                cpu_pct = 100 * (1 - idle_diff / max(total_diff, 1))
            else:
                cpu_pct = 0
            self._last_cpu = (cpu_total, cpu_idle)

            # Memory usage
            with open('/proc/meminfo', 'r') as f:
                mem_lines = f.readlines()[:3]
            mem_total = int(mem_lines[0].split()[1]) / 1024 / 1024  # GB
            mem_avail = int(mem_lines[2].split()[1]) / 1024 / 1024  # GB
            mem_used = mem_total - mem_avail
            mem_pct = 100 * mem_used / mem_total

            # GPU usage (nvidia-smi) - show both compute activity and VRAM
            try:
                result = subprocess.run(
                    ['nvidia-smi', '--query-gpu=utilization.gpu,memory.used,memory.total', '--format=csv,noheader,nounits'],
                    capture_output=True, text=True, timeout=1
                )
                if result.returncode == 0:
                    parts = result.stdout.strip().split(', ')
                    gpu_compute = int(parts[0])  # Compute activity % (spikes during inference)
                    gpu_mem_used = int(parts[1]) / 1024  # GB
                    gpu_mem_total = int(parts[2]) / 1024  # GB
                    vram_pct = 100 * gpu_mem_used / gpu_mem_total
                    # Show compute% and VRAM usage
                    gpu_str = f"GPU:{gpu_compute:3d}% VRAM:{gpu_mem_used:.1f}/{gpu_mem_total:.0f}G({vram_pct:.0f}%)"
                else:
                    gpu_str = ""
            except:
                gpu_str = ""

            # Format display
            stats = f"CPU:{cpu_pct:3.0f}% RAM:{mem_used:.1f}/{mem_total:.0f}G({mem_pct:.0f}%)"
            if gpu_str:
                stats += f" {gpu_str}"

            self.sys_label.set_markup(f'<span font="monospace 8" foreground="#aaaaaa">{stats}</span>')
        except Exception as e:
            self.sys_label.set_markup(f'<span font="monospace 8" foreground="#ff6666">err</span>')

        return True  # Keep timer running

    def set_button_color(self, btn, color):
        css = Gtk.CssProvider()
        text_color = "black" if color in ["#00ff00", "#ffff00"] else "white"
        css.load_from_data(f'''
            button {{
                background: {color};
                background-image: none;
                color: {text_color};
                border: none;
                border-radius: 4px;
                padding: 8px 12px;
                margin: 4px;
            }}
        '''.encode())
        ctx = btn.get_style_context()
        for provider in getattr(btn, '_css_providers', []):
            ctx.remove_provider(provider)
        btn._css_providers = [css]
        ctx.add_provider(css, Gtk.STYLE_PROVIDER_PRIORITY_USER)

    def show_status(self):
        if self.status_window is None:
            self.status_window = StatusWindow()
        self.status_window.label.set_text(f"Processing... ({self.processing_count})")
        self.status_window.show_all()

    def hide_status(self):
        if self.status_window:
            self.status_window.hide()

    def update_status(self):
        if self.processing_count > 0:
            self.show_status()
        else:
            self.hide_status()

    def on_button_clicked(self, button, name, filename):
        # Capture button: first click = screenshot + record, second click = stop
        if name == "capture":
            if not self.capture_recording:
                self.start_capture(button)
            else:
                self.stop_capture()
            return

        if self.recording_process is None:
            self.start_recording(button, name, filename)
        elif self.current_button == button:
            self.stop_recording()

    def start_capture(self, button):
        """Take screenshot then start recording audio"""
        self.set_button_color(button, self.colors["recording"])
        self.capture_button = button
        self.capture_use_claude = self.use_claude  # Capture claude mode state

        # Take screenshot first (in thread to not block UI)
        thread = threading.Thread(target=self.capture_screenshot_thread)
        thread.daemon = True
        thread.start()

    def capture_screenshot_thread(self):
        """Take screenshot, then start audio recording"""
        import tempfile

        # Take screenshot
        with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as f:
            self.capture_screenshot_path = f.name
        self.log(f"[capture] Screenshot path: {self.capture_screenshot_path}")

        try:
            self.log("[capture] Running slurp...")
            result = subprocess.run(["slurp"], capture_output=True, text=True)
            if result.returncode != 0:
                # User cancelled with Esc
                self.log(f"[capture] slurp cancelled/failed: {result.returncode}")
                if os.path.exists(self.capture_screenshot_path):
                    os.unlink(self.capture_screenshot_path)
                self.capture_screenshot_path = None
                GLib.idle_add(self.cancel_capture)
                return
            region = result.stdout.strip()
            self.log(f"[capture] Region: {region}")
            subprocess.run(["grim", "-g", region, self.capture_screenshot_path], check=True)
            self.log(f"[capture] Screenshot saved: {os.path.exists(self.capture_screenshot_path)}")
        except Exception as e:
            # Failed to capture
            self.log(f"[capture] Error: {e}")
            if self.capture_screenshot_path and os.path.exists(self.capture_screenshot_path):
                os.unlink(self.capture_screenshot_path)
            self.capture_screenshot_path = None
            GLib.idle_add(self.cancel_capture)
            return

        # Start audio recording
        self.capture_audio_path = tempfile.mktemp(suffix=".wav")
        self.capture_recording = subprocess.Popen(
            ["arecord", "-D", AUDIO_DEVICE, "-f", "cd", self.capture_audio_path],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            start_new_session=True
        )

    def cancel_capture(self):
        """Cancel capture and reset button"""
        self.set_button_color(self.capture_button, self.colors["capture"])
        self.capture_button = None
        self.capture_recording = None
        self.capture_screenshot_path = None
        self.capture_audio_path = None
        return False

    def stop_capture(self):
        """Stop recording and process capture"""
        self.log(f"[capture] stop_capture called, recording={self.capture_recording is not None}")
        if self.capture_recording:
            try:
                self.capture_recording.send_signal(signal.SIGINT)
                self.capture_recording.wait(timeout=3)
            except:
                # Force kill if stuck
                try:
                    self.capture_recording.kill()
                except:
                    pass
            self.capture_recording = None

            # Reset button color
            self.set_button_color(self.capture_button, self.colors["capture"])

            # Process in background
            self.processing_count += 1
            GLib.idle_add(self.update_status)

            screenshot_path = self.capture_screenshot_path
            audio_path = self.capture_audio_path
            use_claude = getattr(self, 'capture_use_claude', False)
            self.capture_screenshot_path = None
            self.capture_audio_path = None
            self.capture_button = None
            self.capture_use_claude = False

            thread = threading.Thread(
                target=self.process_capture_thread,
                args=(screenshot_path, audio_path, use_claude)
            )
            thread.daemon = True
            thread.start()

    def process_capture_thread(self, screenshot_path, audio_path, use_claude=False):
        """Transcribe audio, analyze with VL model or Claude, execute response"""
        import base64
        import json
        import urllib.request
        import time

        self.log(f"[capture] process_capture_thread started: screenshot={screenshot_path}, audio={audio_path}, claude={use_claude}")

        llama_pids = []
        try:
            # Check audio file
            audio_size = os.path.getsize(audio_path) if os.path.exists(audio_path) else 0
            self.log(f"[capture] Audio file size: {audio_size} bytes")

            # Suspend llama-server to free VRAM for whisper
            self.log(f"[capture] Suspending llama-server...")
            result = subprocess.run(["pgrep", "-f", "llama-server"], capture_output=True, text=True)
            if result.returncode == 0:
                llama_pids = [int(pid) for pid in result.stdout.strip().split('\n') if pid]
                for pid in llama_pids:
                    try:
                        os.kill(pid, signal.SIGSTOP)
                    except:
                        pass
                time.sleep(0.5)  # Give GPU time to release

            # Transcribe audio
            self.log(f"[capture] Converting audio...")
            converted_path = audio_path.replace(".wav", "_16k.wav")
            subprocess.run(
                ["ffmpeg", "-y", "-i", audio_path, "-ar", "16000", "-ac", "1", converted_path],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL,
                start_new_session=True
            )

            self.log(f"[capture] Running whisper...")
            output_base = audio_path.replace(".wav", "")
            whisper_result = subprocess.run(
                [WHISPER_CLI, "-m", MODEL, "-f", converted_path,
                 "-otxt", "-of", output_base,
                 "-ng",  # Use GPU
                 "-t", "4"],
                capture_output=True,
                text=True,
                stdin=subprocess.DEVNULL,
                start_new_session=True
            )
            if whisper_result.returncode != 0:
                self.log(f"[capture] Whisper error: {whisper_result.stderr[:200] if whisper_result.stderr else 'unknown'}")

            transcript_path = output_base + ".txt"
            transcript = ""
            if os.path.exists(transcript_path):
                with open(transcript_path, "r") as f:
                    transcript = f.read().strip()
                os.unlink(transcript_path)
            self.log(f"[capture] Transcript: '{transcript[:50]}...' " if len(transcript) > 50 else f"[capture] Transcript: '{transcript}'")

            # Resume llama-server after whisper
            for pid in llama_pids:
                try:
                    os.kill(pid, signal.SIGCONT)
                except:
                    pass

            # Cleanup audio files
            for p in [audio_path, converted_path]:
                if os.path.exists(p):
                    os.unlink(p)

            if not transcript:
                self.log("[capture] No transcript - aborting")
                if os.path.exists(screenshot_path):
                    os.unlink(screenshot_path)
                return

            # ========== CLAUDE MODE ==========
            if use_claude:
                self.log(f"[* Claude capture mode - sending image + transcript to Claude")
                # Use claude CLI with image file - Claude can read images via Read tool
                # Format: "Read this image file: /path/to/image.png and then: <user instruction>"
                prompt = f"First, read and analyze this image file: {screenshot_path}\n\nThen respond to this instruction: {transcript}"

                try:
                    result = subprocess.run(
                        ["claude", "-p", prompt, "--permission-mode", "acceptEdits", "--allowedTools", "Read,Bash,Glob,Grep,Edit,Write"],
                        capture_output=True,
                        text=True,
                        timeout=300,
                        cwd=os.path.dirname(screenshot_path)  # Run from screenshot dir
                    )
                    self.log(f"[claude-capture] {result.stdout}")
                    if result.stderr:
                        self.log(f"[claude-capture-err] {result.stderr}")
                except subprocess.TimeoutExpired:
                    self.log("[! Claude capture request timed out")
                except Exception as e:
                    self.log(f"[! Claude capture failed: {e}")
                finally:
                    # Clean up screenshot after Claude processes it
                    if os.path.exists(screenshot_path):
                        os.unlink(screenshot_path)
                return

            # ========== LOCAL VL MODEL MODE ==========
            # Kill any existing LLM servers to free VRAM
            subprocess.run(["pkill", "-f", "llama-server"], stderr=subprocess.DEVNULL)
            time.sleep(2)

            # Start VL server with mmproj for vision support
            VL_PORT = 9091
            VL_MODEL = os.path.expanduser("~/workspace/models/Qwen3-VL-8B-Instruct-Q8_0.gguf")
            MMPROJ = os.path.expanduser("~/workspace/models/mmproj-Qwen3VL-8B-Instruct-F16.gguf")

            self.log(f"[*] Starting VL server with mmproj...")
            vl_process = subprocess.Popen(
                ["llama-server", "-m", VL_MODEL, "--mmproj", MMPROJ, "-ngl", "99", "-c", "4096", "--port", str(VL_PORT)],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL,
                start_new_session=True
            )

            server_ready = False
            for _ in range(120):
                try:
                    urllib.request.urlopen(f"http://localhost:{VL_PORT}/health", timeout=2)
                    server_ready = True
                    break
                except:
                    time.sleep(1)

            if not server_ready:
                self.log("[! VL server failed to start")
                vl_process.terminate()
                return

            # Analyze with vision model using OpenAI-compatible API
            with open(screenshot_path, "rb") as f:
                image_data = base64.b64encode(f.read()).decode("utf-8")

            prompt = """Describe what you see in this image in detail.
If it's code/terminal: describe the language, visible code, errors, file paths.
If it's a UI: describe the elements, text, and layout.
If it's something else: describe what you actually see.
Be accurate - only describe what is truly visible."""

            # Use OpenAI-compatible chat completions API with base64 data URL
            request_data = {
                "model": "gpt-4-vision-preview",
                "messages": [
                    {
                        "role": "user",
                        "content": [
                            {
                                "type": "image_url",
                                "image_url": {
                                    "url": f"data:image/png;base64,{image_data}"
                                }
                            },
                            {
                                "type": "text",
                                "text": prompt
                            }
                        ]
                    }
                ],
                "max_tokens": 1024,
                "temperature": 0.3
            }

            req = urllib.request.Request(
                f"http://localhost:{VL_PORT}/v1/chat/completions",
                data=json.dumps(request_data).encode("utf-8"),
                headers={"Content-Type": "application/json"}
            )

            try:
                self.log("[*] Waiting for VL response...")
                with urllib.request.urlopen(req, timeout=180) as response:
                    result = json.loads(response.read().decode("utf-8"))
                    # OpenAI chat completions format
                    vl_response = result.get("choices", [{}])[0].get("message", {}).get("content", "")
            except Exception as e:
                self.log(f"[!] VL request failed: {e}")
                vl_process.terminate()
                return

            # Shut down VL server
            vl_process.terminate()
            vl_process.wait()
            time.sleep(1)

            # Log the VL model output so user can see it
            vl_description = vl_response.strip()
            self.log("=" * 40)
            self.log("[VL MODEL OUTPUT]")
            self.log("-" * 40)
            self.log(vl_description)
            self.log("-" * 40)
            self.log(f"[USER COMMAND] {transcript}")
            self.log("=" * 40)

            # Format: [SCREENSHOT] description + user's command
            content = f"[SCREENSHOT CONTEXT]\n{vl_description}\n[END SCREENSHOT]\n\nUser request: {transcript}"

            filepath = os.path.join(TRANSCRIPT_DIR, "main.txt")
            with open(filepath, "w") as f:
                f.write(content)

            self.log("[*] Sent to coding agent")

        finally:
            # Make sure llama-server is resumed
            for pid in llama_pids:
                try:
                    os.kill(pid, signal.SIGCONT)
                except:
                    pass
            if os.path.exists(screenshot_path):
                os.unlink(screenshot_path)
            GLib.idle_add(self.on_capture_done)

    def on_capture_done(self):
        self.processing_count -= 1
        self.update_status()
        return False

    def start_recording(self, button, name, filename):
        self.current_button = button
        self.current_name = name
        self.current_file = os.path.join(TRANSCRIPT_DIR, filename)
        self.temp_wav = f"/tmp/recording_{name}_{os.getpid()}.wav"

        self.set_button_color(button, self.colors["recording"])

        self.recording_process = subprocess.Popen(
            ["arecord", "-D", AUDIO_DEVICE, "-f", "cd", self.temp_wav],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            stdin=subprocess.DEVNULL,
            start_new_session=True
        )

    def stop_recording(self):
        if self.recording_process:
            try:
                self.recording_process.send_signal(signal.SIGINT)
                self.recording_process.wait(timeout=3)
            except:
                # Force kill if stuck
                try:
                    self.recording_process.kill()
                except:
                    pass
            self.recording_process = None

            # Store values for thread
            temp_wav = self.temp_wav
            current_file = self.current_file
            current_name = self.current_name
            use_claude = self.use_claude  # Capture claude mode state

            # Reset button color immediately
            btn, filename, color = self.buttons[current_name]
            self.set_button_color(btn, color)

            # Reset state so new recording can start
            self.current_button = None
            self.current_name = None
            self.current_file = None
            self.temp_wav = None

            # Start transcription in background thread
            self.processing_count += 1
            GLib.idle_add(self.update_status)

            thread = threading.Thread(
                target=self.transcribe_thread,
                args=(temp_wav, current_file, use_claude)
            )
            thread.daemon = True
            thread.start()

    def transcribe_thread(self, temp_wav, output_file, use_claude=False):
        llama_pids = []
        try:
            if not os.path.exists(temp_wav):
                return

            # Suspend llama-server to free VRAM for whisper GPU
            result = subprocess.run(["pgrep", "-f", "llama-server"], capture_output=True, text=True)
            if result.returncode == 0:
                llama_pids = [int(p) for p in result.stdout.strip().split('\n') if p]
                for pid in llama_pids:
                    try:
                        os.kill(pid, signal.SIGSTOP)
                    except:
                        pass
                import time
                time.sleep(0.5)  # Let VRAM free up

            # Convert to 16kHz mono
            converted_wav = temp_wav.replace(".wav", "_16k.wav")
            subprocess.run(
                ["ffmpeg", "-y", "-i", temp_wav, "-ar", "16000", "-ac", "1", converted_wav],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL,
                start_new_session=True
            )

            # Run whisper with GPU acceleration
            output_base = output_file.replace(".txt", "")
            subprocess.run(
                [WHISPER_CLI, "-m", MODEL, "-f", converted_wav,
                 "-otxt", "-of", output_base,
                 "-ng",  # Use GPU
                 "-t", "4",  # 4 threads for CPU fallback parts
                ],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                stdin=subprocess.DEVNULL,
                start_new_session=True
            )

            # Prepend metadata to transcript
            if os.path.exists(output_file):
                with open(output_file, 'r') as f:
                    transcript = f.read()
                with open(output_file, 'w') as f:
                    prefix = ""
                    if use_claude:
                        prefix += "[CLAUDE] "
                    f.write(f"{prefix}{transcript}")

            # Cleanup
            if os.path.exists(temp_wav):
                os.remove(temp_wav)
            if os.path.exists(converted_wav):
                os.remove(converted_wav)

        finally:
            # Resume llama-server
            for pid in llama_pids:
                try:
                    os.kill(pid, signal.SIGCONT)
                except:
                    pass
            GLib.idle_add(self.on_transcribe_done)

    def on_transcribe_done(self):
        self.processing_count -= 1
        self.update_status()
        return False

def main():
    win = TranscriptButtons()
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    Gtk.main()

if __name__ == "__main__":
    main()
