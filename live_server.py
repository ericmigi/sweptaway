#!/usr/bin/env python3
"""Live ~30 fps MJPEG view of a running Pebble emulator.

Frames are pulled from the QEMU monitor's `screendump` (PPM), converted to JPEG
with Pillow, and pushed as a multipart/x-mixed-replace stream. The target
emulator's monitor port is auto-discovered by machine name, and the frame size
is read from each PPM header, so it works for any platform (emery, gabbro, ...).
Bound to 0.0.0.0 so it is reachable over Tailscale.
"""
import glob
import io
import os
import re
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from PIL import Image

PORT = int(os.environ.get("LIVE_PORT", "8088"))
MACHINE = os.environ.get("EMU_MACHINE", "pebble-emery")
LABEL = os.environ.get("EMU_LABEL", "emery (Obelix)")
DUMP = "/tmp/sweepers_frame.ppm"
FPS = 30
SCALE = 2

_lock = threading.Lock()
_jpeg = b""
_seq = 0
_dims = (0, 0)


def discover_monitor_port(machine):
    """Find the -monitor tcp::PORT of the qemu-pebble running `machine`."""
    for cmdpath in glob.glob("/proc/*/cmdline"):
        try:
            with open(cmdpath, "rb") as f:
                cmd = f.read().replace(b"\x00", b" ").decode("utf-8", "ignore")
        except OSError:
            continue
        if "qemu" in cmd and ("machine " + machine) in cmd:
            m = re.search(r"-monitor tcp::(\d+)", cmd)
            if m:
                return int(m.group(1))
    return None


def monitor_connect():
    port = discover_monitor_port(MACHINE)
    if port is None:
        raise RuntimeError("no running emulator for " + MACHINE)
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    s.settimeout(2.0)
    time.sleep(0.2)
    try:
        s.recv(4096)
    except Exception:
        pass
    return s


def ppm_size(path):
    """Return (header_len, w, h) for a P6 PPM, or None if not yet complete."""
    try:
        with open(path, "rb") as f:
            head = f.read(40)
    except OSError:
        return None
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", head)
    if not m:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    return (m.end(), w, h)


def grab(sock):
    try:
        if os.path.exists(DUMP):
            os.unlink(DUMP)
    except OSError:
        pass
    sock.sendall(b"screendump " + DUMP.encode() + b"\n")
    deadline = time.time() + 1.0
    while time.time() < deadline:
        info = ppm_size(DUMP)
        if info:
            hlen, w, h = info
            try:
                if os.path.getsize(DUMP) == hlen + w * h * 3:
                    with open(DUMP, "rb") as f:
                        return f.read(), (w, h)
            except OSError:
                pass
        time.sleep(0.003)
    return None, None


def capture_loop():
    global _jpeg, _seq, _dims
    period = 1.0 / FPS
    sock = None
    while True:
        start = time.time()
        try:
            if sock is None:
                sock = monitor_connect()
            ppm, dims = grab(sock)
            try:
                sock.recv(65536)
            except Exception:
                pass
            if ppm:
                img = Image.open(io.BytesIO(ppm)).convert("RGB")
                if SCALE != 1:
                    img = img.resize((img.width * SCALE, img.height * SCALE),
                                     Image.NEAREST)
                buf = io.BytesIO()
                img.save(buf, format="JPEG", quality=80)
                with _lock:
                    _jpeg = buf.getvalue()
                    _seq += 1
                    _dims = dims
        except Exception:
            try:
                if sock:
                    sock.close()
            except Exception:
                pass
            sock = None
            time.sleep(0.5)
        dt = time.time() - start
        if dt < period:
            time.sleep(period - dt)


def page():
    w, h = _dims if _dims != (0, 0) else (200, 228)
    vw, vh = w * 2, h * 2
    return ("""<!doctype html>
<html><head><meta charset="utf-8"><title>Sweeper's Clock - live</title>
<style>
  body{background:#111;color:#ccc;font-family:system-ui,sans-serif;text-align:center;margin:0;padding:24px}
  h1{font-weight:600;font-size:18px;letter-spacing:.04em}
  #wrap{display:inline-block;padding:16px;background:#000;border-radius:14px;box-shadow:0 0 50px #0008}
  img{width:%dpx;height:%dpx;image-rendering:pixelated;border-radius:10px;display:block}
  a{color:#9cf;font-size:15px;text-decoration:none}
</style></head>
<body>
  <h1>Sweeper&#39;s Clock &mdash; %s (live, 30fps)</h1>
  <div id="wrap"><img src="/stream.mjpeg" alt="emulator"></div>
  <p><a id="dl" href="#" download>&#11015; Download sweepers-clock.pbw (emery + gabbro)</a></p>
<script>
  document.getElementById('dl').href =
    location.protocol + '//' + location.hostname + ':8090/sweepers-clock.pbw';
</script>
</body></html>""" % (vw, vh, LABEL)).encode()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/stream.mjpeg"):
            self.send_response(200)
            self.send_header("Age", "0")
            self.send_header("Cache-Control", "no-cache, private")
            self.send_header("Pragma", "no-cache")
            self.send_header("Content-Type",
                             "multipart/x-mixed-replace; boundary=frame")
            self.end_headers()
            last = -1
            try:
                while True:
                    with _lock:
                        data, seq = _jpeg, _seq
                    if data and seq != last:
                        last = seq
                        self.wfile.write(b"--frame\r\n")
                        self.wfile.write(b"Content-Type: image/jpeg\r\n")
                        self.wfile.write(
                            ("Content-Length: %d\r\n\r\n" % len(data)).encode())
                        self.wfile.write(data)
                        self.wfile.write(b"\r\n")
                    time.sleep(1.0 / (FPS + 5))
            except (BrokenPipeError, ConnectionResetError):
                return
        elif self.path.startswith("/screen.jpg"):
            with _lock:
                data = _jpeg
            if not data:
                self.send_response(503)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", "image/jpeg")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
        else:
            body = page()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)


if __name__ == "__main__":
    threading.Thread(target=capture_loop, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    print(f"Serving live MJPEG of {MACHINE} on http://0.0.0.0:{PORT}")
    srv.serve_forever()
