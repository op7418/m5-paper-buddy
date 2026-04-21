#!/usr/bin/env python3
"""Bridge Claude Code ↔ M5Paper buddy.

Stands in for the Claude Desktop app. Hook flow:

  Claude Code hook  ──POST──▶  this daemon  ──serial/BLE──▶  M5Paper
                                     ▲                           │
                                     └───── permission ack ──────┘

Two transports:
  - USB serial: zero-setup, autodetects /dev/cu.usbserial-*.
  - BLE (Nordic UART Service via bleak): wireless. First connect triggers
    macOS's system pairing dialog — enter the 6-digit passkey shown on
    the Paper. After that, the daemon auto-reconnects whenever both sides
    are alive.

Heartbeat extensions vs the stock desktop protocol (firmware ignores
unknown fields, so this stays backward compatible):
  project / branch / dirty   — session's git context
  budget                      — daily token budget bar
  model                       — current Claude model
  assistant_msg               — last prose reply pulled from transcript
  prompt.body                 — full approval content (diff / full command)
  prompt.kind                 — "permission" or "question"
  prompt.options              — AskUserQuestion options (rendered as buttons)

Usage:
    python3 tools/claude_code_bridge.py                    # auto: serial first, else BLE
    python3 tools/claude_code_bridge.py --transport ble    # force BLE
    python3 tools/claude_code_bridge.py --transport serial # force serial
    python3 tools/claude_code_bridge.py --budget 1000000
"""

import argparse
import asyncio
import glob
import json
import os
import hashlib
import base64
import struct as _struct
import subprocess
import sys
import threading
import time
import queue
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, HTTPServer

# Nordic UART Service UUIDs — match the firmware's ble_bridge.cpp.
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX_UUID      = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # central → device (write)
NUS_TX_UUID      = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # device → central (notify)

# -----------------------------------------------------------------------------
# Shared state
# -----------------------------------------------------------------------------

STATE_LOCK = threading.Lock()

SESSIONS_RUNNING = set()
SESSIONS_TOTAL   = set()
SESSIONS_WAITING = set()
SESSION_META     = {}          # sid -> {cwd, project, branch, dirty, checked_at}
TRANSCRIPT       = deque(maxlen=8)
TOKENS_TOTAL     = 0
TOKENS_TODAY     = 0
ACTIVE_PROMPT    = None        # currently-focused prompt shown on device
PENDING_PROMPTS  = {}          # prompt_id -> prompt dict (all unresolved)
PENDING          = {}          # prompt_id -> {"event", "decision"}

BUDGET_LIMIT        = 0
MODEL_NAME          = ""
ASSISTANT_MSG       = ""                # global fallback when no session is focused
SESSION_ASSISTANT   = {}                # sid -> latest assistant text (per-session)
SESSION_TRANSCRIPTS = {}                # sid -> deque(maxlen=50) of {role,text,time}
SESSION_STOPPED_AT  = {}                # sid -> timestamp when Stop hook fired
FOCUSED_SID         = None              # user-picked focused session (for dashboard)
TRANSPORT           = None
BUMP_EVENT          = threading.Event()

# WebSocket clients for web buddy
WS_CLIENTS  = []           # list of {"send_queue": queue.Queue}
WS_LOCK     = threading.Lock()
WS_ACTIVE_SOCKETS = set()  # sockets owned by WS threads — server must not close

SERVED_HTML = ""           # loaded from web_buddy.html at startup


def log(*a, **kw):
    print(*a, file=sys.stderr, flush=True, **kw)


def now_hm():
    return datetime.now().strftime("%H:%M")


def add_transcript(line: str):
    with STATE_LOCK:
        TRANSCRIPT.appendleft(f"{now_hm()} {line[:80]}")


# -----------------------------------------------------------------------------
# Transport abstraction. Device I/O is line-based JSON — transports deliver
# bytes one at a time via an on_byte() callback and accept full frames via
# write(). A line buffer lives in the daemon (below), not in the transport.
# -----------------------------------------------------------------------------

class Transport:
    def start(self, on_byte, on_connect=None): raise NotImplementedError
    def write(self, data: bytes): raise NotImplementedError
    def connected(self) -> bool: raise NotImplementedError


class SerialTransport(Transport):
    def __init__(self, port):
        import serial
        self._port_name = port
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self._write_lock = threading.Lock()
        time.sleep(0.2)   # let the port settle before talking
        log(f"[serial] opened {port}")

    def start(self, on_byte, on_connect=None):
        if on_connect:
            on_connect()   # serial is "connected" as soon as the port opens
        threading.Thread(target=self._reader, args=(on_byte,), daemon=True).start()

    def _reader(self, on_byte):
        while True:
            try:
                chunk = self.ser.read(256)
            except Exception as e:
                log(f"[serial] read fail: {e}")
                time.sleep(1)
                continue
            for b in chunk:
                on_byte(b)

    def write(self, data: bytes):
        with self._write_lock:
            try:
                self.ser.write(data)
            except Exception as e:
                log(f"[serial] write fail: {e}")

    def connected(self): return True


class BLETransport(Transport):
    """BLE Central via bleak.

    Runs an asyncio event loop on a dedicated thread. Scans for a device
    advertising a name starting with "Claude-", connects, subscribes to
    the Nordic UART TX characteristic for notifications, and exposes a
    thread-safe write() that marshals back onto the asyncio loop.

    Reconnects automatically on disconnect or scan failure.
    """
    def __init__(self, name_prefix="Claude-"):
        self._name_prefix = name_prefix
        self._loop  = None
        self._client = None
        self._thread = None
        self._on_byte = None
        self._on_connect = None
        self._connected_evt = threading.Event()

    def start(self, on_byte, on_connect=None):
        self._on_byte = on_byte
        self._on_connect = on_connect
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self):
        try:
            self._loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self._loop)
            self._loop.run_until_complete(self._main())
        except Exception as e:
            log(f"[ble] thread crashed: {e!r}")

    async def _main(self):
        try:
            from bleak import BleakScanner, BleakClient
        except ImportError:
            log("[ble] bleak not installed. run: pip install bleak")
            return

        while True:
            log(f"[ble] scanning for '{self._name_prefix}*'...")
            device = None
            try:
                device = await BleakScanner.find_device_by_filter(
                    lambda d, ad: bool(d.name) and d.name.startswith(self._name_prefix),
                    timeout=10.0,
                )
            except Exception as e:
                log(f"[ble] scan error: {e}")

            if not device:
                log("[ble] no device found, retrying in 5s")
                await asyncio.sleep(5)
                continue

            log(f"[ble] connecting to {device.name} ({device.address})")
            try:
                # Bleak's context manager handles disconnect on exit. We stay
                # inside it as long as the link is alive.
                async with BleakClient(device) as client:
                    self._client = client

                    def _on_notify(_sender, data: bytearray):
                        for b in data:
                            self._on_byte(b)
                    await client.start_notify(NUS_TX_UUID, _on_notify)

                    self._connected_evt.set()
                    log("[ble] connected")
                    # Fire the connect callback on a SEPARATE thread. Calling
                    # it inline here deadlocks: the callback does sync writes
                    # that marshal back onto this asyncio loop, but the loop
                    # is blocked waiting for the callback to return.
                    if self._on_connect:
                        threading.Thread(
                            target=self._on_connect, daemon=True,
                            name="ble-handshake",
                        ).start()

                    while client.is_connected:
                        await asyncio.sleep(1.0)
                    log("[ble] link lost")
            except Exception as e:
                log(f"[ble] client error: {e!r}")
            finally:
                self._client = None
                self._connected_evt.clear()

            await asyncio.sleep(2)

    def write(self, data: bytes):
        client = self._client
        if client is None or not client.is_connected:
            return
        try:
            fut = asyncio.run_coroutine_threadsafe(
                client.write_gatt_char(NUS_RX_UUID, data, response=False),
                self._loop,
            )
            fut.result(timeout=3)
        except Exception as e:
            log(f"[ble] write fail: {e!r}")

    def connected(self): return self._connected_evt.is_set()


# -----------------------------------------------------------------------------
# WebSocket helpers (RFC 6455) — zero external dependencies.
# -----------------------------------------------------------------------------

WS_GUID = "258EAFA5-E914-47DA-95CA-5AB5ABE7E1D9"


def ws_accept_key(key: str) -> str:
    return base64.b64encode(
        hashlib.sha1((key + WS_GUID).encode()).digest()
    ).decode()


def ws_encode(data: bytes, opcode: int = 1) -> bytes:
    """Encode a WebSocket frame (server → client, no mask)."""
    frame = bytearray()
    frame.append(0x80 | opcode)          # FIN + opcode
    length = len(data)
    if length < 126:
        frame.append(length)
    elif length < 65536:
        frame.append(126)
        frame.extend(_struct.pack("!H", length))
    else:
        frame.append(127)
        frame.extend(_struct.pack("!Q", length))
    frame.extend(data)
    return bytes(frame)


def ws_decode(buf: bytearray):
    """Try to decode one WebSocket frame from *buf*.
    Returns (payload_bytes, opcode, bytes_consumed) or None if incomplete.
    """
    if len(buf) < 2:
        return None
    opcode = buf[0] & 0x0F
    masked = bool(buf[1] & 0x80)
    length = buf[1] & 0x7F
    offset = 2
    if length == 126:
        if len(buf) < 4:
            return None
        length = _struct.unpack("!H", buf[2:4])[0]
        offset = 4
    elif length == 127:
        if len(buf) < 10:
            return None
        length = _struct.unpack("!Q", buf[2:10])[0]
        offset = 10
    if masked:
        if len(buf) < offset + 4:
            return None
        mask = buf[offset:offset + 4]
        offset += 4
    if len(buf) < offset + length:
        return None
    payload = bytearray(buf[offset:offset + length])
    if masked:
        for i in range(length):
            payload[i] ^= mask[i % 4]
    return (bytes(payload), opcode, offset + length)


# -----------------------------------------------------------------------------
# Line-based RX parsing — transport delivers bytes, we assemble JSON lines.
# -----------------------------------------------------------------------------

_rx_buf = bytearray()


def on_json_obj(obj: dict):
    """Handle a parsed JSON command from any source (hardware or WebSocket)."""
    global FOCUSED_SID
    cmd = obj.get("cmd")
    if cmd == "permission":
        pid = obj.get("id")
        h = PENDING.get(pid)
        if h:
            h["decision"] = obj.get("decision")
            h["event"].set()
    elif cmd == "focus_session":
        FOCUSED_SID = obj.get("sid") or None
        BUMP_EVENT.set()
        # Immediately send transcript for the focused session — don't wait
        # for the heartbeat loop (avoids 1s+ delay on click).
        _send_session_detail(FOCUSED_SID)
    elif cmd == "prompt":
        text = obj.get("text", "").strip()
        sid = obj.get("sid", "")
        if text and sid:
            threading.Thread(target=_send_prompt, args=(sid, text), daemon=True).start()


def _send_session_detail(sid: str):
    """Immediately push the full transcript for a session to all WS clients."""
    if not sid or sid not in SESSION_TRANSCRIPTS:
        return
    detail = json.dumps({
        "session_detail": {
            "sid": sid,
            "transcript": list(SESSION_TRANSCRIPTS[sid]),
        }
    }, separators=(",", ":"), ensure_ascii=False) + "\n"
    with WS_LOCK:
        dead = []
        for i, client in enumerate(WS_CLIENTS):
            try:
                client["send_q"].put_nowait(detail)
            except Exception:
                dead.append(i)
        for i in reversed(dead):
            WS_CLIENTS.pop(i)


def _send_prompt(sid: str, text: str):
    """Send a user prompt to a stopped Claude Code session via claude --continue.

    Only works for sessions that have already stopped (status=done).
    Running sessions cannot be injected into — claude locks the session file.
    """
    # Check if session is still running
    if sid in SESSIONS_RUNNING:
        log(f"[prompt] session {sid[:8]} is still running, cannot inject prompt")
        _append_session_transcript(sid, "system", "Cannot send prompt — session is still running")
        BUMP_EVENT.set()
        _send_session_detail(sid)
        return

    add_transcript(f"> {text[:60]}")
    _append_session_transcript(sid, "user", text[:2000])
    BUMP_EVENT.set()
    _send_session_detail(sid)
    log(f"[prompt] sending to session {sid[:8]}: {text[:60]}")
    try:
        import shutil
        claude_bin = shutil.which("claude") or "claude"
        env = os.environ.copy()
        if os.name == "nt" and not env.get("CLAUDE_CODE_GIT_BASH_PATH"):
            bash = shutil.which("bash")
            if bash:
                fixed = bash.replace("\\usr\\bin\\", "\\bin\\")
                if os.path.isfile(fixed):
                    env["CLAUDE_CODE_GIT_BASH_PATH"] = fixed
                else:
                    env["CLAUDE_CODE_GIT_BASH_PATH"] = bash
        meta = SESSION_META.get(sid) or {}
        cwd = meta.get("cwd")
        if cwd and os.path.isdir(cwd):
            log(f"[prompt] using cwd={cwd}")
        else:
            cwd = None
            log(f"[prompt] no cwd for session {sid[:8]}, using default")
        result = subprocess.run(
            [claude_bin, "-p", text, "--continue"],
            capture_output=True, text=True, timeout=120,
            encoding="utf-8", errors="replace",
            shell=(os.name == "nt"),
            env=env,
            cwd=cwd,
        )
        if result.stdout:
            output = result.stdout.strip()
            log(f"[prompt] session {sid[:8]} done, output: {output[:100]}")
            SESSION_ASSISTANT[sid] = output[:5000]
            _append_session_transcript(sid, "assistant", output[:2000])
            BUMP_EVENT.set()
            _send_session_detail(sid)
        if result.returncode != 0:
            log(f"[prompt] exit code {result.returncode}: {result.stderr[:200]}")
    except subprocess.TimeoutExpired:
        log(f"[prompt] session {sid[:8]} timed out")
    except Exception as e:
        log(f"[prompt] failed: {e}")


def _append_session_transcript(sid: str, role: str, text: str):
    """Append a message to a session's per-session transcript."""
    if not sid:
        return
    if sid not in SESSION_TRANSCRIPTS:
        SESSION_TRANSCRIPTS[sid] = deque(maxlen=50)
    SESSION_TRANSCRIPTS[sid].append({
        "role": role,
        "text": text,
        "time": now_hm(),
    })


def on_rx_byte(b: int):
    global _rx_buf
    if b in (0x0A, 0x0D):   # \n or \r
        if _rx_buf:
            raw = bytes(_rx_buf)
            _rx_buf = bytearray()
            try:
                line = raw.decode("utf-8", errors="replace")
            except Exception:
                return
            log(f"[dev<] {line}")
            if not line.startswith("{"):
                return
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                return
            on_json_obj(obj)
    else:
        if len(_rx_buf) < 4096:   # sanity cap; devices don't send this much anyway
            _rx_buf.append(b)


def send_line(obj: dict):
    # UTF-8 goes through as-is now that the firmware loads a CJK TTF
    # and uses UTF-8-aware wrapping. (Prior revision stripped non-ASCII
    # here to work around the default font crashing on multi-byte codes.)
    data = (json.dumps(obj, separators=(",", ":"), ensure_ascii=False) + "\n").encode()
    if TRANSPORT is not None:
        TRANSPORT.write(data)
    # Broadcast to WebSocket clients (async queues — use put_nowait)
    text = data.decode("utf-8", errors="replace")
    with WS_LOCK:
        dead = []
        for i, client in enumerate(WS_CLIENTS):
            try:
                client["send_q"].put_nowait(text)
            except Exception:
                dead.append(i)
        for i in reversed(dead):
            WS_CLIENTS.pop(i)


# -----------------------------------------------------------------------------
# Git / project introspection — unchanged from the previous revision.
# -----------------------------------------------------------------------------

GIT_TTL_SEC = 10


def _git(cwd, *args, timeout=2.0):
    try:
        out = subprocess.run(("git", *args), cwd=cwd, capture_output=True,
                             text=True, timeout=timeout, check=False)
        return out.stdout.strip() if out.returncode == 0 else ""
    except (subprocess.TimeoutExpired, FileNotFoundError, OSError):
        return ""


def refresh_git(sid: str, cwd: str):
    if not cwd or not os.path.isdir(cwd):
        return
    now = time.time()
    meta = SESSION_META.get(sid) or {}
    if meta.get("cwd") == cwd and (now - meta.get("checked_at", 0)) < GIT_TTL_SEC:
        return
    root = _git(cwd, "rev-parse", "--show-toplevel") or cwd
    SESSION_META[sid] = {
        "cwd": cwd,
        "project":    os.path.basename(root.rstrip("/"))[:39] or "",
        "branch":     _git(cwd, "rev-parse", "--abbrev-ref", "HEAD")[:39],
        "dirty":      sum(1 for ln in _git(cwd, "status", "--porcelain").splitlines() if ln.strip()),
        "checked_at": now,
    }


# -----------------------------------------------------------------------------
# Tool → display hint + body
# -----------------------------------------------------------------------------

HINT_FIELDS = {
    "Bash": "command", "Edit": "file_path", "MultiEdit": "file_path",
    "Write": "file_path", "Read": "file_path", "NotebookEdit": "notebook_path",
    "WebFetch": "url", "WebSearch": "query",
    "Glob": "pattern", "Grep": "pattern",
}


def hint_from_tool(tool: str, tin: dict) -> str:
    field = HINT_FIELDS.get(tool)
    if field and isinstance((tin or {}).get(field), str):
        return tin[field]
    for v in (tin or {}).values():
        if isinstance(v, str):
            return v
    return json.dumps(tin or {})[:60]


def body_from_tool(tool: str, tin: dict) -> str:
    tin = tin or {}

    if tool == "AskUserQuestion":
        # Body is just the question text — options are rendered as touch
        # buttons on the device via prompt.options. Don't duplicate here.
        qs = tin.get("questions")
        if isinstance(qs, list) and qs and isinstance(qs[0], dict):
            q = qs[0].get("question") or qs[0].get("header") or ""
        else:
            q = tin.get("question", "")
        return (q or "").strip()[:500]

    if tool == "Bash":
        cmd  = tin.get("command", "")
        desc = tin.get("description", "")
        return (f"{desc}\n\n$ {cmd}" if desc else f"$ {cmd}")[:500]

    if tool in ("Edit", "MultiEdit"):
        path = tin.get("file_path", "")
        oldv = str(tin.get("old_string", ""))[:180]
        newv = str(tin.get("new_string", ""))[:180]
        return f"{path}\n\n--- old\n{oldv}\n\n+++ new\n{newv}"

    if tool == "Write":
        path    = tin.get("file_path", "")
        content = str(tin.get("content", ""))
        head    = content[:320]
        return f"{path}\n\n{head}{('...' if len(content) > 320 else '')}"

    if tool == "Read":
        return tin.get("file_path", "")

    if tool == "WebFetch":
        url = tin.get("url", "")
        prompt = str(tin.get("prompt", ""))[:200]
        return f"{url}\n\n{prompt}" if prompt else url

    if tool == "WebSearch":
        return str(tin.get("query", ""))[:300]

    if tool in ("Glob", "Grep"):
        parts = [f"pattern: {tin.get('pattern', '')}"]
        if tin.get("path"): parts.append(f"path: {tin['path']}")
        if tin.get("type"): parts.append(f"type: {tin['type']}")
        return "\n".join(parts)[:300]

    try:
        return json.dumps(tin, indent=2)[:500]
    except Exception:
        return str(tin)[:500]


# -----------------------------------------------------------------------------
# Heartbeat construction
# -----------------------------------------------------------------------------

def build_heartbeat() -> dict:
    with STATE_LOCK:
        msg = (f"approve: {ACTIVE_PROMPT['tool']}" if ACTIVE_PROMPT
               else (TRANSCRIPT[0][6:] if TRANSCRIPT else "idle"))
        # tokens_today is now "focused session's current context" — gets
        # filled in below once we resolve which session is focused. Start
        # with zero; a session without transcript data will stay at zero.
        hb = {
            "total":        len(SESSIONS_TOTAL),
            "running":      len(SESSIONS_RUNNING),
            "idle":         len(SESSIONS_RUNNING) == 0,
            "last_event":   TRANSCRIPT[0][:80] if TRANSCRIPT else "",
            "waiting":      len(SESSIONS_WAITING),
            "msg":          msg[:23],
            "entries":      list(TRANSCRIPT),
            "tokens":       0,
            "tokens_today": 0,
        }
        if ACTIVE_PROMPT:
            p = {
                "id":   ACTIVE_PROMPT["id"],
                "tool": ACTIVE_PROMPT["tool"][:19],
                "hint": ACTIVE_PROMPT["hint"][:43],
                "body": ACTIVE_PROMPT["body"][:500],
                "kind": ACTIVE_PROMPT.get("kind", "permission"),
            }
            opts = ACTIVE_PROMPT.get("option_labels") or []
            if opts: p["options"] = opts[:4]
            # Identify which session this prompt is from — so the user
            # can see on the Paper which project/window needs an answer.
            sid = ACTIVE_PROMPT.get("session_id", "")
            if sid:
                p["sid"] = sid[:8]
                meta = SESSION_META.get(sid) or {}
                p["project"] = meta.get("project", "")[:23]
            hb["prompt"] = p

        # Waiting count (for the "N waiting" indicator); approval cards
        # FIFO out of this queue so we don't need to ship the full list.
        # (Earlier revisions sent a `pending[]` tab strip — removed, user
        # preferred dashboard-level session switching over approval tabs.)

        # sessions array: one entry per session with rich status data.
        sessions_list = []
        for sid in list(SESSIONS_TOTAL):
            meta = SESSION_META.get(sid) or {}
            is_running = sid in SESSIONS_RUNNING
            is_waiting = sid in SESSIONS_WAITING
            stopped = SESSION_STOPPED_AT.get(sid)
            status = "waiting" if is_waiting else ("working" if is_running else "done")
            sessions_list.append({
                "sid":        sid[:8],
                "full":       sid,
                "proj":       (meta.get("project", "") or "")[:22],
                "branch":     (meta.get("branch", "") or "")[:16],
                "dirty":      meta.get("dirty", 0),
                "status":     status,
                "model":      SESSION_MODEL.get(sid, ""),
                "latest":     SESSION_ASSISTANT.get(sid, "")[:100],
                "tokens":     SESSION_CONTEXT.get(sid, 0),
                "stopped_at": stopped,
                "focused":    sid == FOCUSED_SID,
            })
        if sessions_list:
            hb["sessions"] = sessions_list
        if BUDGET_LIMIT > 0:   hb["budget"] = BUDGET_LIMIT

        # Resolve which session "focuses" the dashboard view. Priority:
        # 1. User tap (FOCUSED_SID) if still valid
        # 2. Session that raised the current approval
        # 3. Most recently-active running session
        sid = None
        if FOCUSED_SID and FOCUSED_SID in SESSION_META:
            sid = FOCUSED_SID
        elif ACTIVE_PROMPT and ACTIVE_PROMPT.get("session_id"):
            sid = ACTIVE_PROMPT["session_id"]
        elif SESSIONS_RUNNING:
            sid = next(iter(SESSIONS_RUNNING))
        elif SESSION_META:
            sid = max(SESSION_META, key=lambda s: SESSION_META[s].get("checked_at", 0))

        if sid and sid in SESSION_META:
            m = SESSION_META[sid]
            hb["project"] = m.get("project", "")
            hb["branch"]  = m.get("branch", "")
            hb["dirty"]   = m.get("dirty", 0)

        # Per-session current-turn context usage → the number the budget
        # bar on the device should compare against the model's window.
        if sid:
            ctx = SESSION_CONTEXT.get(sid, 0)
            hb["tokens"] = ctx
            hb["tokens_today"] = ctx

        # Model from the focused session's transcript. Fall back to the
        # legacy global (rarely populated since hook payloads don't carry
        # a `model` field).
        s_model = SESSION_MODEL.get(sid) if sid else None
        if s_model:       hb["model"] = s_model
        elif MODEL_NAME:   hb["model"] = MODEL_NAME

        a_msg = SESSION_ASSISTANT.get(sid) if sid else None
        if a_msg:   hb["assistant_msg"] = a_msg
        elif ASSISTANT_MSG: hb["assistant_msg"] = ASSISTANT_MSG

        # Send full transcript for the focused session
        detail_sid = FOCUSED_SID or sid
        if detail_sid and detail_sid in SESSION_TRANSCRIPTS:
            hb["session_detail"] = {
                "sid": detail_sid,
                "transcript": list(SESSION_TRANSCRIPTS[detail_sid]),
            }
    return hb


def heartbeat_loop():
    """Send a heartbeat on BUMP (state change) or every 10s if idle.

    Rate-limited to one send per MIN_INTERVAL seconds so a busy second
    window firing hooks constantly doesn't flood the device — the ESP32
    would get stuck trying to parse + redraw every delta and eventually
    hang the watchdog. Bumps during the quiet window are coalesced into
    the next send (the clear-then-wait pattern picks up any new set).
    """
    MIN_INTERVAL = 1.0
    last_sent = 0.0
    while True:
        BUMP_EVENT.wait(timeout=10)
        BUMP_EVENT.clear()
        now = time.time()
        since = now - last_sent
        if since < MIN_INTERVAL:
            time.sleep(MIN_INTERVAL - since)
        send_line(build_heartbeat())
        last_sent = time.time()


# -----------------------------------------------------------------------------
# Model + transcript helpers (unchanged)
# -----------------------------------------------------------------------------

def short_model(full: str) -> str:
    if not full: return ""
    import re
    s = full.lower()
    family = "Claude"
    for tag, label in (("opus", "Opus"), ("sonnet", "Sonnet"), ("haiku", "Haiku")):
        if tag in s:
            family = label; break
    m = re.search(r"(\d+)[\.\-](\d+)", s)
    if m: return f"{family} {m.group(1)}.{m.group(2)}"
    return family if family != "Claude" else full[:28]


def extract_session_context(path: str) -> int:
    """Return the session's CURRENT context-window usage, approximated
    as (last assistant turn's input_tokens + output_tokens). Hook-scope
    "tokens today" across all sessions isn't useful to a user — they
    want to see how full the context window for THIS session is.
    """
    if not path or not os.path.exists(path):
        return 0
    try:
        sz = os.path.getsize(path)
        with open(path, "rb") as f:
            f.seek(max(0, sz - 131072))
            data = f.read()
        lines = data.decode("utf-8", errors="replace").splitlines()
        for line in reversed(lines):
            line = line.strip()
            if not line or not line.startswith("{"): continue
            try: obj = json.loads(line)
            except json.JSONDecodeError: continue
            msg = obj.get("message", obj)
            if not isinstance(msg, dict) or msg.get("role") != "assistant":
                continue
            usage = msg.get("usage")
            if isinstance(usage, dict):
                inp = int(usage.get("input_tokens", 0) or 0)
                out = int(usage.get("output_tokens", 0) or 0)
                # input_tokens already accounts for the rolled-up
                # conversation state (cache-read counted separately but
                # included in input_tokens as of CC's schema).
                return inp + out
    except Exception:
        pass
    return 0


# Per-session context-window usage (updated on each hook).
SESSION_CONTEXT: dict = {}


def extract_session_model(path: str) -> str:
    """Find the most recent assistant message in the transcript and
    return its `model` field. Hook payloads don't carry model info;
    transcripts do (per assistant turn)."""
    if not path or not os.path.exists(path):
        return ""
    try:
        sz = os.path.getsize(path)
        with open(path, "rb") as f:
            f.seek(max(0, sz - 131072))
            data = f.read()
        lines = data.decode("utf-8", errors="replace").splitlines()
        for line in reversed(lines):
            line = line.strip()
            if not line or not line.startswith("{"): continue
            try: obj = json.loads(line)
            except json.JSONDecodeError: continue
            msg = obj.get("message", obj)
            if not isinstance(msg, dict) or msg.get("role") != "assistant":
                continue
            m = msg.get("model")
            if isinstance(m, str) and m:
                return m
    except Exception:
        pass
    return ""


SESSION_MODEL: dict = {}


def extract_last_assistant(path: str) -> str:
    if not path or not os.path.exists(path):
        return ""
    try:
        sz = os.path.getsize(path)
        with open(path, "rb") as f:
            f.seek(max(0, sz - 131072))
            data = f.read()
        lines = data.decode("utf-8", errors="replace").splitlines()
        for line in reversed(lines):
            line = line.strip()
            if not line or not line.startswith("{"):
                continue
            try: obj = json.loads(line)
            except json.JSONDecodeError: continue
            msg = obj.get("message", obj)
            if not isinstance(msg, dict): continue
            if msg.get("role") != "assistant": continue
            content = msg.get("content")
            text = ""
            if isinstance(content, str):
                text = content
            elif isinstance(content, list):
                for block in content:
                    if isinstance(block, dict) and block.get("type") == "text":
                        text = block.get("text", "")
                        if text: break
            text = (text or "").strip()
            if text:
                return text[:5000]
    except Exception as e:
        log(f"[transcript] error: {e}")
    return ""


# -----------------------------------------------------------------------------
# HTTP handler — unchanged in terms of semantics.
# -----------------------------------------------------------------------------

class HookHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args): pass

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/":
            self._serve_html()
        elif path == "/manifest.json":
            self._serve_manifest()
        else:
            self.send_response(404)
            self.end_headers()

    def _serve_html(self):
        body = SERVED_HTML.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except BrokenPipeError: pass

    def _serve_manifest(self):
        manifest = json.dumps({
            "name": "Paper Buddy",
            "short_name": "Buddy",
            "start_url": "/",
            "display": "standalone",
            "orientation": "portrait",
            "background_color": "#ffffff",
            "theme_color": "#ffffff",
            "icons": [{
                "src": "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>📟</text></svg>",
                "sizes": "192x192",
                "type": "image/svg+xml",
            }],
        })
        body = manifest.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except BrokenPipeError: pass

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length") or "0")
            body = self.rfile.read(n) if n > 0 else b""
            payload = json.loads(body.decode("utf-8")) if body else {}
        except Exception as e:
            return self._reply(400, {"error": str(e)})

        event = payload.get("hook_event_name", "")
        log(f"[hook] {event} session={payload.get('session_id', '')[:8]}")

        sid = payload.get("session_id", "")
        cwd = payload.get("cwd", "")
        # Auto-register sessions that started before the daemon
        if sid and sid not in SESSIONS_TOTAL:
            with STATE_LOCK:
                SESSIONS_TOTAL.add(sid)
                if sid not in SESSION_TRANSCRIPTS:
                    SESSION_TRANSCRIPTS[sid] = deque(maxlen=50)
            if event not in ("SessionStart", "Stop"):
                with STATE_LOCK:
                    SESSIONS_RUNNING.add(sid)
            log(f"[hook] auto-registered session {sid[:8]}")
        if sid and cwd:
            refresh_git(sid, cwd)

        global MODEL_NAME, ASSISTANT_MSG
        for k in ("model", "model_id", "assistant_model"):
            v = payload.get(k)
            if isinstance(v, str) and v:
                MODEL_NAME = short_model(v); break

        tp = payload.get("transcript_path")
        if isinstance(tp, str) and tp:
            if sid:
                m = extract_session_model(tp)
                if m:
                    SESSION_MODEL[sid] = short_model(m)
            latest = extract_last_assistant(tp)
            if latest:
                if sid and SESSION_ASSISTANT.get(sid) != latest:
                    SESSION_ASSISTANT[sid] = latest
                    _append_session_transcript(sid, "assistant", latest)
                    BUMP_EVENT.set()
                if latest != ASSISTANT_MSG:
                    ASSISTANT_MSG = latest
                    BUMP_EVENT.set()
            if sid:
                ctx = extract_session_context(tp)
                if SESSION_CONTEXT.get(sid) != ctx:
                    SESSION_CONTEXT[sid] = ctx
                    BUMP_EVENT.set()

        try:
            if   event == "SessionStart":      resp = self._session_start(payload)
            elif event == "Stop":              resp = self._session_stop(payload)
            elif event == "UserPromptSubmit":  resp = self._user_prompt(payload)
            elif event == "PreToolUse":        resp = self._pretool(payload)
            elif event == "PostToolUse":       resp = self._posttool(payload)
            else:                              resp = {}
        except Exception as e:
            log(f"[hook] handler error: {e!r}"); resp = {}

        self._reply(200, resp)

    def _reply(self, code: int, obj: dict):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except BrokenPipeError: pass

    def _session_start(self, p):
        sid = p.get("session_id", "")
        with STATE_LOCK:
            SESSIONS_TOTAL.add(sid); SESSIONS_RUNNING.add(sid)
            if sid not in SESSION_TRANSCRIPTS:
                SESSION_TRANSCRIPTS[sid] = deque(maxlen=50)
        proj = (SESSION_META.get(sid) or {}).get("project", "")
        add_transcript(f"session: {proj}" if proj else "session started")
        _append_session_transcript(sid, "system", f"session started" + (f": {proj}" if proj else ""))
        BUMP_EVENT.set()
        return {}

    def _session_stop(self, p):
        sid = p.get("session_id", "")
        with STATE_LOCK:
            SESSIONS_RUNNING.discard(sid)
        SESSION_STOPPED_AT[sid] = time.time()
        add_transcript("session done")
        _append_session_transcript(sid, "system", "session done — waiting for input")
        BUMP_EVENT.set()
        return {}

    def _user_prompt(self, p):
        prompt = (p.get("prompt") or "").strip().replace("\n", " ")
        sid = p.get("session_id", "")
        if prompt:
            # Skip if _send_prompt already added this exact message (avoid duplicate)
            dq = SESSION_TRANSCRIPTS.get(sid)
            if dq:
                last = dq[-1] if dq else {}
                if last.get("role") == "user" and last.get("text","")[:200] == prompt[:200]:
                    return {}
            add_transcript(f"> {prompt[:60]}")
            _append_session_transcript(sid, "user", prompt[:200])
            BUMP_EVENT.set()
        return {}

    def _posttool(self, p):
        tool = p.get("tool_name", "?")
        sid = p.get("session_id", "")
        add_transcript(f"{tool} done")
        _append_session_transcript(sid, "system", f"{tool} done")
        BUMP_EVENT.set()
        return {}

    def _pretool(self, p):
        global ACTIVE_PROMPT
        sid  = p.get("session_id", "")
        tool = p.get("tool_name", "?")
        tin  = p.get("tool_input") or {}

        if p.get("permission_mode") == "bypassPermissions":
            add_transcript(f"{tool} (bypass)")
            BUMP_EVENT.set()
            return {"hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "allow",
                "permissionDecisionReason": "bypass-permissions mode",
            }}

        # Auto-approve all tools except AskUserQuestion — the web buddy
        # is a notification hub, not an approval gate.
        if tool != "AskUserQuestion":
            add_transcript(f"{tool}")
            BUMP_EVENT.set()
            return {"hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "allow",
                "permissionDecisionReason": "auto-approved (web buddy)",
            }}

        hint = hint_from_tool(tool, tin)
        body = body_from_tool(tool, tin)

        kind = "question" if tool == "AskUserQuestion" else "permission"
        option_labels = []
        if kind == "question":
            qs = tin.get("questions")
            if isinstance(qs, list) and qs and isinstance(qs[0], dict):
                for o in (qs[0].get("options") or [])[:4]:
                    option_labels.append(str(o.get("label")) if isinstance(o, dict) else str(o))
            else:
                for o in (tin.get("options") or [])[:4]:
                    option_labels.append(str(o.get("label")) if isinstance(o, dict) else str(o))

        prompt_id = f"req_{int(time.time() * 1000)}_{os.getpid()}"
        event = threading.Event()
        holder = {"event": event, "decision": None}
        PENDING[prompt_id] = holder

        prompt_obj = {
            "id": prompt_id, "tool": tool, "hint": hint, "body": body,
            "kind": kind, "option_labels": option_labels, "session_id": sid,
        }

        global ACTIVE_PROMPT
        with STATE_LOCK:
            SESSIONS_WAITING.add(sid)
            PENDING_PROMPTS[prompt_id] = prompt_obj
            if ACTIVE_PROMPT is None:
                ACTIVE_PROMPT = prompt_obj
        BUMP_EVENT.set()

        try:
            got = event.wait(timeout=30)
            decision = holder["decision"] if got else None
            if isinstance(decision, str) and decision.startswith("option:"):
                time.sleep(0.6)
        finally:
            PENDING.pop(prompt_id, None)
            with STATE_LOCK:
                SESSIONS_WAITING.discard(sid)
                PENDING_PROMPTS.pop(prompt_id, None)
                if ACTIVE_PROMPT and ACTIVE_PROMPT["id"] == prompt_id:
                    ACTIVE_PROMPT = next(iter(PENDING_PROMPTS.values()), None)
            BUMP_EVENT.set()

        if isinstance(decision, str) and decision.startswith("option:"):
            try: idx = int(decision.split(":", 1)[1])
            except ValueError: idx = -1
            label = option_labels[idx] if 0 <= idx < len(option_labels) else ""
            add_transcript(f"{tool} → {label[:30]}"); BUMP_EVENT.set()
            return {"hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "deny",
                "permissionDecisionReason": (
                    f"The user answered on the M5Paper buddy device: "
                    f"'{label}' (option {idx + 1}). Proceed using this answer "
                    f"directly — do NOT call AskUserQuestion again."
                ),
            }}

        if decision == "once":
            add_transcript(f"{tool} allow"); BUMP_EVENT.set()
            return {"hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "allow",
                "permissionDecisionReason": "Approved on M5Paper",
            }}
        if decision == "deny":
            add_transcript(f"{tool} deny"); BUMP_EVENT.set()
            reason = ("The user cancelled this question on the M5Paper "
                      "buddy without answering. Ask them directly in the "
                      "terminal instead.") if kind == "question" else "Denied on M5Paper"
            return {"hookSpecificOutput": {
                "hookEventName": "PreToolUse",
                "permissionDecision": "deny",
                "permissionDecisionReason": reason,
            }}
        add_transcript(f"{tool} timeout"); BUMP_EVENT.set()
        return {}


class BuddyHTTPServer(HTTPServer):
    """Plain HTTPServer — WebSocket runs on its own websockets server."""
    pass


async def _ws_handler_async(websocket):
    """Async WebSocket handler called by websockets.serve()."""
    import asyncio
    send_q = asyncio.Queue()
    client_info = {"send_q": send_q, "ws": websocket}
    with WS_LOCK:
        WS_CLIENTS.append(client_info)
    log("[ws] client connected")

    # Send handshake + initial heartbeat
    owner = ""
    try: owner = os.environ.get("USER", "")
    except Exception: pass
    if owner:
        await send_q.put('{"cmd":"owner","name":"' + owner + '"}\n')
    await send_q.put('{"time":[' + str(int(time.time())) + ',' + str(tz_offset_seconds()) + ']}\n')
    await send_q.put(json.dumps(build_heartbeat(), separators=(",", ":"), ensure_ascii=False) + "\n")

    async def sender():
        """Pull from queue and send to websocket."""
        try:
            while True:
                text = await send_q.get()
                if text is None:
                    break
                await websocket.send(text)
        except Exception:
            pass

    sender_task = asyncio.create_task(sender())

    try:
        async for message in websocket:
            text = message if isinstance(message, str) else message.decode("utf-8", errors="replace")
            for line in text.split("\n"):
                line = line.strip()
                if not line or not line.startswith("{"):
                    continue
                try:
                    obj = json.loads(line)
                    on_json_obj(obj)
                except json.JSONDecodeError:
                    pass
    except Exception:
        pass
    finally:
        sender_task.cancel()
        with WS_LOCK:
            try: WS_CLIENTS.remove(client_info)
            except ValueError: pass
        log("[ws] client disconnected")


def _start_ws_server(port):
    """Run the WebSocket server on its own port in a daemon thread."""
    import websockets as _ws_lib
    import asyncio

    async def _serve():
        async with _ws_lib.serve(_ws_handler_async, "0.0.0.0", port):
            log(f"[ws] WebSocket server on port {port}")
            await asyncio.Future()  # run forever

    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    loop.run_until_complete(_serve())


# -----------------------------------------------------------------------------

def tz_offset_seconds() -> int:
    now = time.time()
    local = datetime.fromtimestamp(now)
    utc_dt = datetime(*datetime.fromtimestamp(now, tz=None).utctimetuple()[:6])
    return int((local - utc_dt).total_seconds())


def pick_transport(kind: str) -> Transport:
    """Resolve --transport flag to a concrete Transport. 'auto' tries
    serial first (zero-setup, no BLE permission dance) and falls back
    to BLE if no USB device is found."""
    candidates = sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/ttyUSB*"))

    if kind == "serial":
        if not candidates:
            sys.exit("--transport serial requested but no /dev/cu.usbserial-* found")
        return SerialTransport(candidates[0])

    if kind == "ble":
        return BLETransport()

    # auto
    if candidates:
        log("[transport] serial device found, using USB")
        return SerialTransport(candidates[0])
    log("[transport] no serial device, falling back to BLE")
    return BLETransport()


def main():
    global BUDGET_LIMIT, TRANSPORT, SERVED_HTML

    ap = argparse.ArgumentParser()
    ap.add_argument("--port", help="explicit serial port (implies --transport serial)")
    ap.add_argument("--transport", choices=("auto", "serial", "ble"), default="auto")
    ap.add_argument("--http-port", type=int, default=9876)
    ap.add_argument("--host", default=None,
                    help="bind address (default: 127.0.0.1, 0.0.0.0 when --web)")
    ap.add_argument("--web", action="store_true",
                    help="enable web buddy mode (serve HTML + WebSocket)")
    ap.add_argument("--owner", default=os.environ.get("USER", ""))
    ap.add_argument("--budget", type=int, default=200000,
                    help="context-window limit for the budget bar (default 200K = "
                         "Claude 4.6 standard context; set 1000000 for 1M-context "
                         "beta; set 0 to hide the bar)")
    args = ap.parse_args()

    BUDGET_LIMIT = max(0, args.budget)

    # Load web frontend HTML
    html_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web_buddy.html")
    if os.path.exists(html_path):
        with open(html_path, "r", encoding="utf-8") as f:
            SERVED_HTML = f.read()
        log(f"[web] loaded {html_path} ({len(SERVED_HTML)} bytes)")

    # Transport: if --web and no explicit port/transport, skip hardware
    use_hardware = not args.web or args.port or args.transport != "auto"

    if args.port:
        TRANSPORT = SerialTransport(args.port)
    elif args.web and not args.port and args.transport == "auto":
        # Web-only mode: no hardware transport needed
        log("[transport] web mode, skipping hardware transport")
        TRANSPORT = None
    else:
        TRANSPORT = pick_transport(args.transport)

    # Send the owner + time-sync handshake whenever we (re)connect. For
    # serial, the transport fires on_connect immediately. For BLE, it
    # fires after subscribing to TX notify so the device is ready.
    if TRANSPORT:
        def _handshake():
            if args.owner:
                send_line({"cmd": "owner", "name": args.owner})
            send_line({"time": [int(time.time()), tz_offset_seconds()]})
            send_line(build_heartbeat())

        TRANSPORT.start(on_rx_byte, on_connect=_handshake)

    threading.Thread(target=heartbeat_loop, daemon=True).start()

    host = args.host or ("0.0.0.0" if args.web else "127.0.0.1")
    srv = BuddyHTTPServer((host, args.http_port), HookHandler)
    log(f"[http] listening on {host}:{args.http_port}  budget={BUDGET_LIMIT}")

    # Start WebSocket server on a separate port for web buddy
    ws_port = args.http_port + 1
    if args.web:
        threading.Thread(target=_start_ws_server, args=(ws_port,), daemon=True).start()
        # Show local IPs for convenience
        try:
            import socket as _socket
            s = _socket.socket(_socket.AF_INET, _socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            s.close()
            log(f"[web] open http://{local_ip}:{args.http_port}/ on your device")
            log(f"[web] WebSocket on port {ws_port}")
        except Exception:
            log(f"[web] open http://<your-ip>:{args.http_port}/ on your device")
    log("[ready] start a Claude Code session with the hooks installed")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log("\n[exit] bye")


if __name__ == "__main__":
    main()
