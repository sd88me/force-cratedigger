#!/usr/bin/env python3
"""Force Crate Digger web control panel — serves the UI (web/index.html, a
vintage-Akai-MPC-themed page; see README.md for credit to the original
schwung-webstream project this is built on) and bridges its Discogs-filter/
transport/gain actions to cratedigger_host's Unix control socket (SET/GET/
DESCRIBE — see src/cratedigger_host.cpp's header comment for the protocol,
including the keys added on the Force side: `search_results_json` and
`play_result_index`). The engine's other providers (YouTube/SoundCloud/
archive.org/Freesound search) are still present but this addon's own UI
only ever sets `cratedig_filter` — see cratedigger_host.cpp's main().

Deliberately stdlib-only (http.server + socket): no pip install step needed
on-device, matching this project's other web panels (force-maze/web/
server.py, ~/.claude/skills/mockbamod-module-creator/references/web-gui.md).

Run: python3 server.py [--port N] [--ctrl-sock PATH]
"""
import json
import socket
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlparse, parse_qs

WEB_DIR = Path(__file__).resolve().parent
CTRL_SOCK = "/tmp/cratedigger_ctrl.sock"
SOCK_TIMEOUT = 4.0  # search round-trips (yt-dlp) are slower than a knob turn


def ctrl_request(line: str, timeout: float = SOCK_TIMEOUT):
    """Send one line to cratedigger_host's control socket, return its reply
    (or None if the engine isn't reachable). One connection per request -
    `with` guarantees the socket closes on every exit path, including a
    connect/send/recv timeout (see force-maze/web/server.py's own comment
    for why that matters under a sustained burst of calls)."""
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
            s.settimeout(timeout)
            s.connect(CTRL_SOCK)
            s.sendall((line.strip("\n") + "\n").encode("utf-8"))
            chunks = []
            while True:
                chunk = s.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
                if chunk.endswith(b"\n"):
                    break
            return b"".join(chunks).decode("utf-8", errors="replace").strip("\n")
    except OSError:
        return None


class Handler(BaseHTTPRequestHandler):
    server_version = "ForceCrateDiggerWeb/0.1"

    def log_message(self, fmt, *args):
        sys.stderr.write("[cratedigger-web] " + (fmt % args) + "\n")

    def _text(self, code, body, ctype="text/plain; charset=utf-8"):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _json(self, code, obj):
        self._text(code, json.dumps(obj), "application/json; charset=utf-8")

    def _file(self, relpath, ctype):
        path = WEB_DIR / relpath
        try:
            data = path.read_bytes()
        except OSError:
            self.send_error(404, "not found")
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path

        if path in ("/", "/index.html"):
            self._file("index.html", "text/html; charset=utf-8")
            return

        if path == "/param":
            qs = parse_qs(urlparse(self.path).query)
            key = (qs.get("key") or [""])[0]
            if not key:
                self._text(400, "missing key")
                return
            reply = ctrl_request(f"GET {key}", timeout=1.0)
            if reply is None:
                self._text(503, "engine not running")
            elif reply == "ERR":
                self._text(404, "unknown param")
            else:
                self._text(200, reply)
            return

        if path == "/state":
            # One round-trip bundle for the UI's poll loop: playback state +
            # gain + status, rather than five separate /param round trips.
            keys = [
                "stream_status", "playback_time", "gain", "stream_url",
                "stream_provider", "search_status", "search_count",
                "search_error", "download_status", "download_path",
                "download_error",
            ]
            out = {}
            for k in keys:
                r = ctrl_request(f"GET {k}", timeout=1.0)
                out[k] = None if r in (None, "ERR") else r
            self._json(200, out)
            return

        if path == "/results":
            reply = ctrl_request("GET search_results_json", timeout=1.0)
            if reply is None:
                self._json(503, [])
                return
            try:
                self._json(200, json.loads(reply))
            except json.JSONDecodeError:
                self._json(200, [])
            return

        if path == "/describe":
            reply = ctrl_request("DESCRIBE", timeout=1.0)
            if reply is None:
                self._json(503, {})
            else:
                self._text(200, reply, "application/json; charset=utf-8")
            return

        if path == "/status":
            self._json(200, {"engine_running": ctrl_request("DESCRIBE", timeout=1.0) is not None})
            return

        self.send_error(404, "not found")

    def do_POST(self):
        path = urlparse(self.path).path
        length = int(self.headers.get("Content-Length", 0))
        raw = self.rfile.read(length) if length else b"{}"
        try:
            body = json.loads(raw or b"{}")
        except json.JSONDecodeError:
            self._json(400, {"ok": False, "error": "bad json"})
            return

        if path == "/param":
            key, value = body.get("key"), body.get("value")
            if not key or value is None:
                self._json(400, {"ok": False, "error": "missing key/value"})
                return
            reply = ctrl_request(f"SET {key} {value}", timeout=1.0)
            self._json(200 if reply == "OK" else 503, {"ok": reply == "OK"})
            return

        if path == "/play":
            idx = body.get("index")
            if idx is None:
                self._json(400, {"ok": False, "error": "missing index"})
                return
            reply = ctrl_request(f"SET play_result_index {int(idx)}", timeout=1.0)
            self._json(200 if reply == "OK" else 503, {"ok": reply == "OK"})
            return

        if path == "/download":
            title = (body.get("title") or "cratedigger").replace("\n", " ")
            reply = ctrl_request(f"SET download_wav {title}", timeout=1.0)
            self._json(200 if reply == "OK" else 503, {"ok": reply == "OK"})
            return

        self.send_error(404, "not found")


def main():
    global CTRL_SOCK
    port = 8305  # after force-acid's 8303, force-maze's 8304 — see gotchas.md on port collisions
    args = sys.argv[1:]
    if "--port" in args:
        port = int(args[args.index("--port") + 1])
    if "--ctrl-sock" in args:
        CTRL_SOCK = args[args.index("--ctrl-sock") + 1]

    srv = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"[cratedigger-web] serving on http://0.0.0.0:{port}  (control socket: {CTRL_SOCK})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
