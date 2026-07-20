#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tcp_server.py - Central TCP Server for RK3568 AI Voice System

Runs on Ubuntu server, listens for connections from RK3568 terminal.
Receives audio data and text messages, coordinates with ASR engine
and LLM service, sends AI responses back to the terminal.

Protocol: Custom binary framing (see protocol spec in README).

Usage:
    python3 tcp_server.py [--host 0.0.0.0] [--port 8888]

Dependencies:
    pip install websocket-client requests
"""

import sys
import os
import struct
import socket
import signal
import time
import json
import logging
import argparse
import threading
from pathlib import Path
from datetime import datetime
from typing import Optional, Callable, Tuple, Dict

# Add server directory to path for sibling imports
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Import sibling modules (optional — graceful degradation if not available)
try:
    from asr_voice import XunfeiASR
except ImportError:
    XunfeiASR = None
    print("[tcp_server] WARNING: asr_voice module not found, ASR disabled.")

try:
    from llm_deepseek import DeepSeekChat
except ImportError:
    DeepSeekChat = None
    print("[tcp_server] WARNING: llm_deepseek module not found, LLM disabled.")

# ──────────────────────────────────────────────
#  Constants
# ──────────────────────────────────────────────

DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8888
MAX_CLIENTS = 10
RECV_BUFFER = 8192
HEARTBEAT_INTERVAL = 15      # seconds
HEARTBEAT_TIMEOUT = 45       # seconds — disconnect if no heartbeat
CLIENT_TIMEOUT = 300         # seconds — idle disconnect

# Protocol message types (must match embedded C client)
MSG_AUDIO       = 0x01
MSG_TEXT        = 0x02
MSG_STATUS      = 0x03
MSG_HEARTBEAT   = 0x04
MSG_AI_RESPONSE = 0x05
MSG_ERROR       = 0xFF

# Packet header: msg_type(1) + payload_len(4) + seq_num(4) + checksum(2) = 11 bytes
HEADER_FORMAT = "!B I I H"
HEADER_SIZE   = struct.calcsize(HEADER_FORMAT)

# ──────────────────────────────────────────────
#  Logging Setup
# ──────────────────────────────────────────────

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("tcp_server")

# ──────────────────────────────────────────────
#  Packet Helpers
# ──────────────────────────────────────────────

def build_packet(msg_type: int, payload: bytes = b"") -> bytes:
    """Build a framed packet with header + payload."""
    checksum = 0
    for i, b in enumerate(payload):
        checksum ^= (b << ((i % 2) * 8))
    checksum &= 0xFFFF

    header = struct.pack(HEADER_FORMAT, msg_type, len(payload), 0, checksum)
    return header + payload


def parse_header(data: bytes) -> Optional[Tuple[int, int, int]]:
    """Parse packet header, return (msg_type, payload_len, checksum) or None."""
    if len(data) < HEADER_SIZE:
        return None
    msg_type, payload_len, _seq, checksum = struct.unpack(HEADER_FORMAT, data[:HEADER_SIZE])
    return msg_type, payload_len, checksum


def verify_checksum(payload: bytes, expected: int) -> bool:
    """Verify XOR checksum of payload."""
    checksum = 0
    for i, b in enumerate(payload):
        checksum ^= (b << ((i % 2) * 8))
    return (checksum & 0xFFFF) == expected


# ──────────────────────────────────────────────
#  LLM Session Manager (multi-turn conversation)
# ──────────────────────────────────────────────

class SessionManager:
    """Manages per-client conversation context for multi-turn dialogue."""

    def __init__(self, max_history: int = 20, max_sessions: int = 100):
        self.max_history = max_history
        self.max_sessions = max_sessions
        self.sessions: Dict[str, list] = {}

    def get_history(self, client_id: str) -> list:
        """Retrieve conversation history for a client."""
        if client_id not in self.sessions:
            self.sessions[client_id] = []
        return self.sessions[client_id]

    def add_turn(self, client_id: str, user_msg: str, ai_msg: str):
        """Add a conversation turn to the client's history."""
        if client_id not in self.sessions:
            self.sessions[client_id] = []

        history = self.sessions[client_id]
        history.append({"role": "user", "content": user_msg})
        history.append({"role": "assistant", "content": ai_msg})

        # Trim to max history length
        if len(history) > self.max_history * 2:
            self.sessions[client_id] = history[-(self.max_history * 2):]

        # Prune old sessions if too many
        if len(self.sessions) > self.max_sessions:
            oldest = min(self.sessions.keys(),
                        key=lambda k: len(self.sessions[k]), default=None)
            if oldest and oldest != client_id:
                del self.sessions[oldest]

    def clear(self, client_id: str):
        """Clear conversation history for a client."""
        self.sessions.pop(client_id, None)

    def get_context(self, client_id: str) -> list:
        """Get full conversation context for LLM API call."""
        return self.get_history(client_id)


# ──────────────────────────────────────────────
#  Client Handler
# ──────────────────────────────────────────────

class ClientHandler:
    """Handles a single TCP client connection."""

    def __init__(self, client_sock: socket.socket, addr: Tuple[str, int],
                 asr_engine, llm_engine, session_mgr: SessionManager,
                 audio_dir: str = "./audio_data"):
        self.sock = client_sock
        self.addr = addr
        self.client_id = f"{addr[0]}:{addr[1]}"
        self.asr = asr_engine
        self.llm = llm_engine
        self.sessions = session_mgr
        self.audio_dir = Path(audio_dir)
        self.audio_dir.mkdir(parents=True, exist_ok=True)

        self.alive = True
        self.last_heartbeat = time.time()
        self.recv_buffer = b""
        self.seq_counter = 0

    def send_packet(self, msg_type: int, payload: bytes = b""):
        """Send a framed packet to the client."""
        try:
            packet = build_packet(msg_type, payload)
            self.sock.sendall(packet)
        except (socket.error, BrokenPipeError) as e:
            log.error(f"[{self.client_id}] Send error: {e}")
            self.alive = False

    def send_text(self, text: str):
        """Send a text message to the client."""
        self.send_packet(MSG_AI_RESPONSE, text.encode("utf-8"))

    def send_status(self, status: str):
        """Send a status update to the client."""
        self.send_packet(MSG_STATUS, status.encode("utf-8"))

    def send_error(self, error: str):
        """Send an error message to the client."""
        self.send_packet(MSG_ERROR, error.encode("utf-8"))

    def handle_audio(self, payload: bytes):
        """Process received audio data: save to file, run ASR, then LLM."""
        if not payload:
            return

        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        audio_file = self.audio_dir / f"{self.client_id.replace(':', '_')}_{timestamp}.wav"

        # Write WAV with proper header
        try:
            with open(audio_file, "wb") as f:
                # WAV header for 16kHz 16-bit mono PCM
                data_size = len(payload)
                sample_rate = 16000
                channels = 1
                bits_per_sample = 16
                byte_rate = sample_rate * channels * bits_per_sample // 8
                block_align = channels * bits_per_sample // 8

                f.write(b"RIFF")
                f.write(struct.pack("<I", 36 + data_size))
                f.write(b"WAVE")
                f.write(b"fmt ")
                f.write(struct.pack("<I", 16))          # chunk size
                f.write(struct.pack("<H", 1))            # PCM
                f.write(struct.pack("<H", channels))
                f.write(struct.pack("<I", sample_rate))
                f.write(struct.pack("<I", byte_rate))
                f.write(struct.pack("<H", block_align))
                f.write(struct.pack("<H", bits_per_sample))
                f.write(b"data")
                f.write(struct.pack("<I", data_size))
                f.write(payload)

            log.info(f"[{self.client_id}] Audio saved: {audio_file} ({data_size} bytes)")

        except IOError as e:
            log.error(f"[{self.client_id}] Failed to save audio: {e}")
            self.send_error(f"Failed to save audio: {e}")
            return

        # ASR: Speech to text
        if self.asr:
            self.send_status("正在识别语音...")
            try:
                text = self.asr.recognize(str(audio_file))
                if text:
                    log.info(f"[{self.client_id}] ASR result: {text}")
                    self.send_status(f"识别结果: {text}")
                    # Proceed to LLM
                    self._process_text(text)
                else:
                    self.send_status("语音识别为空，请重试")
            except Exception as e:
                log.error(f"[{self.client_id}] ASR error: {e}")
                self.send_error(f"语音识别失败: {e}")
        else:
            log.warning(f"[{self.client_id}] ASR not available, skipping.")
            self.send_status("ASR 服务未配置")

    def handle_text(self, payload: bytes):
        """Process received text command."""
        try:
            text = payload.decode("utf-8").strip()
        except UnicodeDecodeError:
            log.error(f"[{self.client_id}] Invalid UTF-8 text payload.")
            self.send_error("Invalid text encoding (expected UTF-8)")
            return

        if not text:
            return

        log.info(f"[{self.client_id}] Text received: {text}")

        # Check for control commands
        if text.startswith("/"):
            self._handle_command(text)
            return

        # Regular text → send to LLM
        self._process_text(text)

    def _process_text(self, text: str):
        """Send user text through LLM and return response."""
        if not self.llm:
            log.warning(f"[{self.client_id}] LLM not available.")
            self.send_status("大模型服务未配置，返回原文本")
            self.send_text(f"[ECHO] {text}")
            return

        self.send_status("AI 正在思考...")

        try:
            history = self.sessions.get_context(self.client_id)
            response = self.llm.chat(text, history=history)

            if response:
                self.sessions.add_turn(self.client_id, text, response)
                self.send_text(response)
                log.info(f"[{self.client_id}] AI response sent ({len(response)} chars)")
            else:
                self.send_error("AI 未返回有效回复")

        except Exception as e:
            log.error(f"[{self.client_id}] LLM error: {e}")
            self.send_error(f"AI 处理异常: {e}")

    def _handle_command(self, cmd: str):
        """Handle control commands (prefixed with /)."""
        parts = cmd.split(maxsplit=1)
        command = parts[0].lower()

        if command == "/clear":
            self.sessions.clear(self.client_id)
            self.send_status("对话历史已清除")

        elif command == "/ping":
            self.send_status("pong")

        elif command == "/status":
            history = self.sessions.get_history(self.client_id)
            turns = len(history) // 2
            self.send_status(f"会话轮次: {turns}, 客户端: {self.client_id}")

        elif command == "/help":
            help_text = (
                "可用命令:\n"
                "  /clear  - 清除对话历史\n"
                "  /ping   - 测试连接\n"
                "  /status - 查看会话状态\n"
                "  /help   - 显示此帮助"
            )
            self.send_text(help_text)

        else:
            self.send_status(f"未知命令: {command} (输入 /help 查看帮助)")

    def handle_heartbeat(self, payload: bytes):
        """Handle heartbeat — acknowledge and update timer."""
        self.last_heartbeat = time.time()
        # Echo heartbeat back as acknowledgment
        self.send_packet(MSG_HEARTBEAT, b"\x01")

    def check_timeout(self) -> bool:
        """Check if client has timed out. Returns True if still alive."""
        now = time.time()
        if now - self.last_heartbeat > HEARTBEAT_TIMEOUT:
            log.warning(f"[{self.client_id}] Heartbeat timeout, disconnecting.")
            self.alive = False
            return False
        return True

    def run(self):
        """Main receive loop for this client."""
        self.sock.settimeout(1.0)  # 1-second timeout for responsive shutdown
        self.send_status("已连接到 RK3568 AI 语音系统服务器")

        while self.alive:
            try:
                data = self.sock.recv(RECV_BUFFER)
            except socket.timeout:
                self.check_timeout()
                continue
            except (socket.error, ConnectionResetError) as e:
                log.info(f"[{self.client_id}] Disconnected: {e}")
                break

            if not data:
                log.info(f"[{self.client_id}] Client closed connection.")
                break

            self.recv_buffer += data

            # Parse all complete packets in buffer
            while len(self.recv_buffer) >= HEADER_SIZE:
                result = parse_header(self.recv_buffer)
                if result is None:
                    break

                msg_type, payload_len, checksum = result
                total_size = HEADER_SIZE + payload_len

                if len(self.recv_buffer) < total_size:
                    break  # Wait for more data

                # Extract payload
                payload = self.recv_buffer[HEADER_SIZE:total_size]
                self.recv_buffer = self.recv_buffer[total_size:]

                # Verify checksum (warning only — best-effort)
                if payload and not verify_checksum(payload, checksum):
                    log.warning(f"[{self.client_id}] Checksum mismatch for msg_type=0x{msg_type:02X}")

                # Dispatch by message type
                if msg_type == MSG_AUDIO:
                    self.handle_audio(payload)
                elif msg_type == MSG_TEXT:
                    self.handle_text(payload)
                elif msg_type == MSG_HEARTBEAT:
                    self.handle_heartbeat(payload)
                elif msg_type == MSG_STATUS:
                    log.info(f"[{self.client_id}] Status: {payload.decode('utf-8', errors='replace')}")
                else:
                    log.warning(f"[{self.client_id}] Unknown msg_type=0x{msg_type:02X}")

            # Prevent buffer growing indefinitely (1MB limit)
            if len(self.recv_buffer) > 1_048_576:
                log.error(f"[{self.client_id}] Recv buffer overflow, disconnecting.")
                break

        # Cleanup
        try:
            self.sock.close()
        except Exception:
            pass
        log.info(f"[{self.client_id}] Handler stopped.")


# ──────────────────────────────────────────────
#  TCP Server
# ──────────────────────────────────────────────

class TCPServer:
    """Multi-client TCP server for the RK3568 AI voice system."""

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT):
        self.host = host
        self.port = port
        self.server_sock: Optional[socket.socket] = None
        self.alive = True
        self.clients: Dict[str, ClientHandler] = {}
        self.client_threads: Dict[str, threading.Thread] = {}

        # Initialize engines
        self.asr = None
        self.llm = None
        self._init_engines()

        # Session manager
        self.sessions = SessionManager()

        # Audio storage
        self.audio_dir = Path("./audio_data")

        # Stats
        self.total_connections = 0

    def _init_engines(self):
        """Initialize ASR and LLM engines with configuration."""
        # Load config from file if it exists
        config = {}
        config_path = Path("server_config.json")
        if config_path.exists():
            try:
                with open(config_path, "r", encoding="utf-8") as f:
                    config = json.load(f)
                log.info(f"Loaded config from {config_path}")
            except (json.JSONDecodeError, IOError) as e:
                log.warning(f"Failed to load config: {e}")

        # Initialize ASR (Xunfei)
        if XunfeiASR:
            try:
                asr_config = config.get("asr", {})
                self.asr = XunfeiASR(
                    app_id=asr_config.get("app_id", os.environ.get("XF_APP_ID", "")),
                    api_key=asr_config.get("api_key", os.environ.get("XF_API_KEY", "")),
                    api_secret=asr_config.get("api_secret", os.environ.get("XF_API_SECRET", "")),
                )
                log.info("ASR engine (Xunfei) initialized.")
            except Exception as e:
                log.error(f"Failed to init ASR: {e}")
                self.asr = None

        # Initialize LLM (DeepSeek)
        if DeepSeekChat:
            try:
                llm_config = config.get("llm", {})
                self.llm = DeepSeekChat(
                    api_key=llm_config.get("api_key", os.environ.get("DEEPSEEK_API_KEY", "")),
                    model=llm_config.get("model", "deepseek-chat"),
                    base_url=llm_config.get("base_url", "https://api.deepseek.com"),
                )
                log.info("LLM engine (DeepSeek) initialized.")
            except Exception as e:
                log.error(f"Failed to init LLM: {e}")
                self.llm = None

    def start(self):
        """Start the TCP server and accept connections."""
        self.server_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.server_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

        try:
            self.server_sock.bind((self.host, self.port))
            self.server_sock.listen(MAX_CLIENTS)
            self.server_sock.settimeout(1.0)
        except socket.error as e:
            log.error(f"Failed to bind {self.host}:{self.port}: {e}")
            sys.exit(1)

        log.info(f"╔══════════════════════════════════════════════╗")
        log.info(f"║  RK3568 AI Voice System - TCP Server         ║")
        log.info(f"║  Listening on {self.host}:{self.port}                       ║")
        log.info(f"║  ASR: {'Xunfei ✓' if self.asr else '✗ disabled'}                           ║")
        log.info(f"║  LLM: {'DeepSeek ✓' if self.llm else '✗ disabled'}                         ║")
        log.info(f"╚══════════════════════════════════════════════╝")

        # Main accept loop
        while self.alive:
            try:
                client_sock, addr = self.server_sock.accept()
            except socket.timeout:
                continue
            except Exception as e:
                if self.alive:
                    log.error(f"Accept error: {e}")
                break

            self.total_connections += 1
            log.info(f"[#{self.total_connections}] New connection from {addr[0]}:{addr[1]}")

            # Create handler and spawn thread
            handler = ClientHandler(
                client_sock, addr, self.asr, self.llm,
                self.sessions, str(self.audio_dir),
            )

            client_key = f"{addr[0]}:{addr[1]}"
            # If client reconnects, suffix with connection number
            if client_key in self.clients:
                client_key = f"{addr[0]}:{addr[1]}_c{self.total_connections}"

            self.clients[client_key] = handler

            thread = threading.Thread(
                target=handler.run,
                name=f"client-{client_key}",
                daemon=True,
            )
            thread.start()
            self.client_threads[client_key] = thread

            # Clean up finished threads
            self._reap_finished()

        self.shutdown()

    def _reap_finished(self):
        """Remove finished client handlers."""
        finished = [
            k for k, t in self.client_threads.items()
            if not t.is_alive()
        ]
        for key in finished:
            self.client_threads.pop(key, None)
            self.clients.pop(key, None)

    def shutdown(self):
        """Graceful server shutdown."""
        log.info("Shutting down server...")
        self.alive = False

        # Close all client connections
        for handler in self.clients.values():
            handler.alive = False
            try:
                handler.sock.close()
            except Exception:
                pass

        # Close server socket
        if self.server_sock:
            try:
                self.server_sock.close()
            except Exception:
                pass

        log.info("Server stopped.")


# ──────────────────────────────────────────────
#  Entry Point
# ──────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="RK3568 AI Voice System - TCP Server",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    python3 tcp_server.py
    python3 tcp_server.py --port 9999
    python3 tcp_server.py --host 192.168.1.100 --port 8888

Configuration:
    Set API keys via environment variables:
      export XF_APP_ID="your_xunfei_app_id"
      export XF_API_KEY="your_xunfei_api_key"
      export XF_API_SECRET="your_xunfei_api_secret"
      export DEEPSEEK_API_KEY="your_deepseek_api_key"

    Or create server_config.json:
      {
        "asr": {"app_id": "...", "api_key": "...", "api_secret": "..."},
        "llm": {"api_key": "...", "model": "deepseek-chat"}
      }
        """,
    )
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"Bind address (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"Listen port (default: {DEFAULT_PORT})")
    args = parser.parse_args()

    # Handle graceful signal termination
    server = TCPServer(host=args.host, port=args.port)

    def signal_handler(sig, frame):
        log.info(f"Received signal {sig}, shutting down...")
        server.shutdown()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        server.start()
    except KeyboardInterrupt:
        server.shutdown()
    except Exception as e:
        log.error(f"Fatal error: {e}")
        server.shutdown()
        sys.exit(1)


if __name__ == "__main__":
    main()
