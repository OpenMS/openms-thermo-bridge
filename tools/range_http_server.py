#!/usr/bin/env python3
"""Range-capable static HTTP server for testing remote RAW reading (local use, not run in CI).

usage: range_http_server.py DIR [--port 8765] [--latency-ms 0]

Serves files below DIR with single-range GET support (206 + Content-Range), the subset
S3 implements. --latency-ms delays every response to mimic object-store first-byte latency.
Request and byte counters are printed on SIGINT and served at /__stats (reset with /__reset).
"""
import argparse
import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

STATS = {"requests": 0, "bytes": 0}
LOCK = threading.Lock()


def make_handler(root, latency):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def _json(self, payload):
            body = json.dumps(payload).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = unquote(urlparse(self.path).path)
            if path == "/__stats":
                with LOCK:
                    return self._json(dict(STATS))
            if path == "/__reset":
                with LOCK:
                    STATS.update(requests=0, bytes=0)
                return self._json({"ok": True})
            full = os.path.realpath(os.path.join(root, path.lstrip("/")))
            if not full.startswith(os.path.realpath(root) + os.sep) or not os.path.isfile(full):
                self.send_error(404)
                return
            size = os.path.getsize(full)
            start, end, status = 0, size - 1, 200
            header = self.headers.get("Range")
            if header:
                m = re.fullmatch(r"bytes=(\d*)-(\d*)", header.strip())
                if not m:
                    self.send_error(416)
                    return
                if m.group(1):
                    start = int(m.group(1))
                    end = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
                else:
                    start, end = max(0, size - int(m.group(2))), size - 1
                if start > end or start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                status = 206
            if latency:
                time.sleep(latency)
            length = end - start + 1
            with LOCK:
                STATS["requests"] += 1
                STATS["bytes"] += length
            self.send_response(status)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(length))
            if status == 206:
                self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
            self.end_headers()
            with open(full, "rb") as f:
                f.seek(start)
                remaining = length
                while remaining:
                    chunk = f.read(min(remaining, 1 << 20))
                    if not chunk:
                        break
                    self.wfile.write(chunk)
                    remaining -= len(chunk)

    return Handler


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("root")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--latency-ms", type=float, default=0.0)
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(args.root, args.latency_ms / 1000.0))
    server.daemon_threads = True
    print(f"serving {args.root} on http://127.0.0.1:{args.port}/", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print(json.dumps(STATS))


if __name__ == "__main__":
    main()
