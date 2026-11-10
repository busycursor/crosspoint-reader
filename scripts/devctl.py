#!/usr/bin/env python3
"""Host tool for the firmware's dev control API (src/dev/DevApi.h).

Transports (same endpoints on both):
  default            simulator over HTTP (http://127.0.0.1:8090, token "sim")
  --host IP          device over Wi-Fi (token from ../.devapi-token or $CROSSPOINT_DEV_API_TOKEN)
  --serial PORT      USB serial; PORT = auto (first /dev/cu.usbmodem*), sim (simulator pty) or a path

Commands:
  sim start|stop|status            headless X4 Pro simulator
  state | info                     screen text, heap, Wi-Fi / version, board, SD
  screen [-o shot.png]             framebuffer as PNG
  logs [--since N] [--grep RE]     recent log lines
  wait REGEX [--timeout S]         wait for a log line
  open SCREEN [--path P]           sync|home|settings|library|files|transfer|opds|reader
  input BUTTON                     confirm|back|next|previous
  settings [KEY...]                read settings;  settings-set KEY=VALUE...
  frontlight [--on/--off] [--brightness N] [--warmth N]
  power [sleep|reboot]             battery/USB, or act
  refresh                          full display refresh (clears ghosting)
  wifi [--ssid S --password-file F]
  opds-add --name N --url U [--password-file F] [--header-file F]
  fs ls|get|put|rm|upload PATH [--file LOCAL]
  build [device|sim|all]           build with the dev API token always set (see build())
  flash [FIRMWARE.bin]             upload to the SD card, flash, reboot (device only);
                                   no file = build the device firmware first
"""

from __future__ import annotations

import argparse
import base64
import glob
import json
import os
import re
import secrets
import select
import signal
import struct
import subprocess
import sys
import termios
import time
import tty
import urllib.error
import urllib.parse
import urllib.request
import uuid
import zlib
from pathlib import Path

FIRMWARE = Path(__file__).resolve().parent.parent
WORKSPACE = FIRMWARE.parent
SIM_BINARY = FIRMWARE / ".pio/build/simulator_x4_pro/program"
# Kept outside the firmware repo: the simulated SD card holds obfuscated
# credentials and real book files, and screenshots show library contents.
QA_DIR = WORKSPACE / "qa"
SIM_DIR = QA_DIR / "sim"  # pidfile, log, pty link and the simulated SD card
SIM_PTY = SIM_DIR / "serial"
API_PORT = 8090
TOKEN_FILE = WORKSPACE / ".devapi-token"


class ApiError(RuntimeError):
    pass


# --- transports ---------------------------------------------------------------------

class Client:
    """Common helpers; subclasses implement request() and upload()."""

    def request(self, method: str, path: str, *, query: dict | None = None, body: bytes | None = None,
                content_type: str | None = None) -> tuple[int, dict, bytes]:
        raise NotImplementedError

    def upload(self, device_path: str, data: bytes) -> None:
        raise NotImplementedError

    def json(self, method: str, path: str, payload=None, **kw) -> dict | list:
        body = json.dumps(payload).encode() if payload is not None else None
        code, _, data = self.request(method, path, body=body,
                                     content_type="application/json" if body else None, **kw)
        try:
            doc = json.loads(data or b"{}")
        except json.JSONDecodeError:
            doc = {"raw": data.decode(errors="replace")}
        if code >= 400:
            raise ApiError(f"{method} {path}: HTTP {code} {doc}")
        return doc


class HttpClient(Client):
    def __init__(self, base: str, token: str, timeout: float = 30.0):
        self.base = base.rstrip("/")
        self.token = token
        self.timeout = timeout

    def request(self, method, path, *, query=None, body=None, content_type=None):
        url = self.base + path + ("?" + urllib.parse.urlencode(query) if query else "")
        req = urllib.request.Request(url, data=body, method=method)
        req.add_header("Authorization", f"Bearer {self.token}")
        if content_type:
            req.add_header("Content-Type", content_type)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                return resp.status, dict(resp.headers), resp.read()
        except urllib.error.HTTPError as e:
            return e.code, dict(e.headers), e.read()
        except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
            raise ApiError(f"{method} {path}: {e}") from e

    def upload(self, device_path, data):
        boundary = uuid.uuid4().hex
        name = Path(device_path).name
        body = (f"--{boundary}\r\nContent-Disposition: form-data; name=\"file\"; filename=\"{name}\"\r\n"
                f"Content-Type: application/octet-stream\r\n\r\n").encode() + data + f"\r\n--{boundary}--\r\n".encode()
        code, _, resp = self.request("POST", "/dev/fs/upload", query={"path": device_path}, body=body,
                                     content_type=f"multipart/form-data; boundary={boundary}")
        if code >= 400:
            raise ApiError(f"upload: HTTP {code} {resp!r}")


class SerialClient(Client):
    """Framed request/response over a serial line (see the protocol comment in
    src/dev/DevApi.cpp). Log lines arriving in between are kept in .log_lines."""

    CHUNK = 384
    WINDOW = 4  # body lines in flight before waiting for @@A

    def __init__(self, path: str, timeout: float = 30.0):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[2] |= termios.CLOCAL
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        self.timeout = timeout
        self.buf = b""
        self.log_lines: list[str] = []
        termios.tcflush(self.fd, termios.TCIFLUSH)

    def _write(self, data: bytes) -> None:
        while data:
            select.select([], [self.fd], [], 5)
            try:
                n = os.write(self.fd, data)
            except BlockingIOError:
                continue
            data = data[n:]

    def _line(self, deadline: float) -> str:
        while b"\n" not in self.buf:
            left = deadline - time.time()
            if left <= 0:
                raise ApiError("serial: timed out waiting for the device")
            r, _, _ = select.select([self.fd], [], [], min(left, 1))
            if r:
                try:
                    self.buf += os.read(self.fd, 65536)
                except BlockingIOError:
                    pass
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace").rstrip("\r")

    def _await(self, rid: str, kinds: tuple[str, ...], deadline: float) -> list[str]:
        """Next protocol line for this request whose kind is in kinds."""
        while True:
            line = self._line(deadline)
            parts = line.split(" ")
            if len(parts) >= 2 and parts[0] in kinds and parts[1] == rid:
                return parts
            if not line.startswith("@@"):
                self.log_lines.append(line)

    def request(self, method, path, *, query=None, body=None, content_type=None):
        body = body or b""
        rid = secrets.token_hex(4)
        target = path + ("?" + urllib.parse.urlencode(query) if query else "")
        deadline = time.time() + self.timeout
        self._write(f"@@Q {rid} {method} {target} {len(body)}\n".encode())
        sent = acked = 0
        while sent < len(body):
            # Keep at most WINDOW lines unacknowledged.
            while sent - acked >= self.WINDOW * self.CHUNK:
                acked = int(self._await(rid, ("@@A",), time.time() + self.timeout)[2])
            chunk = body[sent:sent + self.CHUNK]
            self._write(f"@@B {rid} ".encode() + base64.b64encode(chunk) + b"\n")
            sent += len(chunk)
            deadline = time.time() + self.timeout
        parts = self._await(rid, ("@@R",), deadline)
        code, length, ctype = int(parts[2]), int(parts[3]), parts[4]
        headers = {"Content-Type": ctype}
        if len(parts) > 5:
            headers.update(urllib.parse.parse_qsl(parts[5]))
        data = bytearray()
        while True:
            p = self._await(rid, ("@@B", "@@E"), time.time() + self.timeout)
            if p[0] == "@@E":
                break
            data += base64.b64decode(p[2])
        if len(data) != length:
            raise ApiError(f"serial: got {len(data)} of {length} bytes")
        return code, headers, bytes(data)

    def upload(self, device_path, data):
        old = self.timeout
        self.timeout = max(old, 120)
        try:
            code, _, resp = self.request("POST", "/dev/fs/upload", query={"path": device_path}, body=data)
        finally:
            self.timeout = old
        if code >= 400:
            raise ApiError(f"upload: HTTP {code} {resp!r}")


def make_client(args) -> Client:
    serial = getattr(args, "serial", None)
    if serial is not None:
        if serial == "sim":
            path = str(SIM_PTY)
        elif serial and serial != "auto":
            path = serial
        else:
            ports = sorted(glob.glob("/dev/cu.usbmodem*"))
            if not ports:
                sys.exit("no /dev/cu.usbmodem* device; is it plugged in?")
            path = ports[0]
        return SerialClient(path)
    if args.host:
        token = os.environ.get("CROSSPOINT_DEV_API_TOKEN") or (
            TOKEN_FILE.read_text().strip() if TOKEN_FILE.exists() else "")
        if not token:
            sys.exit(f"no token: set CROSSPOINT_DEV_API_TOKEN or create {TOKEN_FILE}")
        return HttpClient(f"http://{args.host}:{API_PORT}", token)
    return HttpClient(f"http://127.0.0.1:{API_PORT}", "sim")


# --- framebuffer -> PNG -------------------------------------------------------------

def framebuffer_to_rows(buf: bytes, width: int, height: int, orientation: int) -> tuple[int, int, list[bytes]]:
    """Map the 1-bit panel buffer (1 = white, MSB first) to logical rows, mirroring
    rotateCoordinates() in lib/GfxRenderer/GfxRenderer.cpp."""
    wb = width // 8
    portrait = orientation in (0, 2)
    lw, lh = (height, width) if portrait else (width, height)

    def phys(x: int, y: int) -> tuple[int, int]:
        if orientation == 0:   # Portrait
            return y, height - 1 - x
        if orientation == 1:   # LandscapeClockwise
            return width - 1 - x, height - 1 - y
        if orientation == 2:   # PortraitInverted
            return width - 1 - y, x
        return x, y            # LandscapeCounterClockwise (native)

    rows = []
    for y in range(lh):
        row = bytearray(lw)
        for x in range(lw):
            px, py = phys(x, y)
            row[x] = 255 if (buf[py * wb + px // 8] >> (7 - px % 8)) & 1 else 0
        rows.append(bytes(row))
    return lw, lh, rows


def write_png(path: Path, width: int, height: int, rows: list[bytes]) -> None:
    def chunk(tag: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + r for r in rows)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(png)


def screenshot(c: Client, out: Path) -> Path:
    code, headers, data = c.request("GET", "/dev/screen")
    if code != 200:
        raise ApiError(f"screen: HTTP {code} {data[:200]!r}")
    h = {k.lower(): v for k, v in headers.items()}
    lw, lh, rows = framebuffer_to_rows(data, int(h["x-width"]), int(h["x-height"]), int(h["x-orientation"]))
    write_png(out, lw, lh, rows)
    return out


def wait_for_log(c: Client, regex: str, since: int | None = None, timeout: float = 60) -> str:
    """Poll logs until regex matches. The firmware stops answering while a long
    operation blocks its loop, so transport errors are retried."""
    pat = re.compile(regex)
    if since is None:
        since = c.json("GET", "/dev/state")["log_seq"]
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            doc = c.json("GET", "/dev/logs", query={"since": since})
        except ApiError:
            time.sleep(1)
            continue
        for line in doc["lines"]:
            if pat.search(line):
                return line
        since = doc["next"]
        time.sleep(0.5)
    raise ApiError(f"timed out after {timeout}s waiting for /{regex}/")


# --- simulator lifecycle -------------------------------------------------------------

def sim_pid() -> int | None:
    pidfile = SIM_DIR / "sim.pid"
    if not pidfile.exists():
        return None
    pid = int(pidfile.read_text().strip() or 0)
    try:
        os.kill(pid, 0)
        return pid
    except OSError:
        return None


def sim_start(args) -> None:
    if sim_pid():
        print("simulator already running")
        return
    if not SIM_BINARY.exists():
        sys.exit(f"build it first: pio run -e simulator_x4_pro  ({SIM_BINARY} missing)")
    sd = Path(args.sd).resolve() if getattr(args, "sd", None) else SIM_DIR / "sd"
    sd.mkdir(parents=True, exist_ok=True)
    SIM_DIR.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, CROSSPOINT_SIM_SD=str(sd), CROSSPOINT_SIM_SCREEN_TEXT="1",
               CROSSPOINT_SIM_SERIAL_PTY=str(SIM_PTY))
    if not getattr(args, "window", False):
        env["SDL_VIDEODRIVER"] = "dummy"
    log = open(SIM_DIR / "sim.log", "w")
    proc = subprocess.Popen([str(SIM_BINARY)], cwd=SIM_DIR, env=env, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    (SIM_DIR / "sim.pid").write_text(str(proc.pid))
    client = HttpClient(f"http://127.0.0.1:{API_PORT}", "sim", timeout=2)
    for _ in range(100):
        time.sleep(0.2)
        if proc.poll() is not None:
            sys.exit(f"simulator exited ({proc.returncode}); see {SIM_DIR / 'sim.log'}")
        try:
            client.json("GET", "/dev/state")
            print(f"simulator up (pid {proc.pid}, SD card {sd}, serial {SIM_PTY})")
            return
        except ApiError:
            continue
    sys.exit("simulator started but the dev API never answered")


def sim_stop(_args=None) -> None:
    pid = sim_pid()
    if not pid:
        print("simulator not running")
        return
    os.killpg(pid, signal.SIGTERM)
    (SIM_DIR / "sim.pid").unlink(missing_ok=True)
    print("simulator stopped")


# --- commands --------------------------------------------------------------------------

def show(doc) -> None:
    print(json.dumps(doc, indent=2))


def parse_value(text: str):
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        return text


def cmd_logs(c, args):
    doc = c.json("GET", "/dev/logs", query={"since": args.since})
    pat = re.compile(args.grep) if args.grep else None
    for line in doc["lines"]:
        if not pat or pat.search(line):
            print(line)
    print(f"# next={doc['next']}" + (" (older lines dropped)" if doc.get("dropped") else ""), file=sys.stderr)


def cmd_settings(c, args):
    query = {"keys": ",".join(args.keys)} if args.keys else None
    for s in c.json("GET", "/dev/settings", query=query):
        extra = ""
        if "options" in s:
            extra = f"  options={s['options']}"
        elif s.get("type") == "value":
            extra = f"  range={s['min']}..{s['max']}"
        print(f"{s['key']} = {json.dumps(s.get('value'))}  [{s['type']}, {s['category']}]{extra}")


def cmd_settings_set(c, args):
    payload = {}
    for pair in args.pairs:
        key, _, value = pair.partition("=")
        payload[key] = parse_value(value)
    show(c.json("POST", "/dev/settings", payload))


def cmd_frontlight(c, args):
    payload = {}
    if args.on is not None:
        payload["on"] = args.on
    if args.brightness is not None:
        payload["brightness"] = args.brightness
    if args.warmth is not None:
        payload["warmth"] = args.warmth
    show(c.json("POST", "/dev/frontlight", payload) if payload else c.json("GET", "/dev/frontlight"))


def cmd_power(c, args):
    show(c.json("POST", "/dev/power", {"action": args.action}) if args.action else c.json("GET", "/dev/power"))


def cmd_wifi(c, args):
    if not args.ssid:
        show(c.json("GET", "/dev/wifi"))
        return
    password = Path(args.password_file).read_text().strip() if args.password_file else ""
    show(c.json("POST", "/dev/wifi", {"ssid": args.ssid, "password": password}))


def cmd_opds_add(c, args):
    payload: dict = {"name": args.name, "url": args.url}
    if args.user:
        payload["username"] = args.user
    if args.password_file:
        creds = Path(args.password_file).read_text().strip()
        # "user:password" (like .cwa-credentials) or a bare password.
        if ":" in creds and not args.user:
            payload["username"], payload["password"] = creds.split(":", 1)
        else:
            payload["password"] = creds
    if args.header_file:
        # Header file: name and value on alternating non-blank lines.
        lines = [l.strip() for l in Path(args.header_file).read_text().splitlines() if l.strip()]
        payload["headers"] = [{"name": lines[i], "value": lines[i + 1]} for i in range(0, len(lines) - 1, 2)]
    c.json("POST", "/dev/opds", payload)
    print(f"saved server {args.name!r}")


def cmd_fs(c, args):
    q = {"path": args.path}
    if args.op == "ls":
        for e in c.json("GET", "/dev/fs", query=q)["entries"]:
            print(("d " if e["dir"] else "f ") + e["name"] + ("" if e["dir"] else f"  {e['size']}"))
    elif args.op == "get":
        code, _, data = c.request("GET", "/dev/fs", query=q)
        if code != 200:
            raise ApiError(f"get: HTTP {code}")
        Path(args.file).write_bytes(data) if args.file else sys.stdout.buffer.write(data)
    elif args.op == "put":
        data = Path(args.file).read_bytes() if args.file else sys.stdin.buffer.read()
        code, _, resp = c.request("PUT", "/dev/fs", query=q, body=data, content_type="text/plain")
        if code >= 400:
            raise ApiError(f"put: HTTP {code} {resp!r}")
        print("written")
    elif args.op == "rm":
        c.json("DELETE", "/dev/fs", query=q)
        print("deleted")
    elif args.op == "upload":
        c.upload(args.path, Path(args.file).read_bytes())
        print("uploaded")


PIO = Path.home() / ".platformio/penv/bin/pio"
BUILD_ENVS = {"device": "x4pro-dev", "sim": "simulator_x4_pro"}
DEVICE_IMAGE = FIRMWARE / ".pio/build/x4pro-dev/firmware.bin"


def build(targets: list[str]) -> None:
    """Build firmware environments. CROSSPOINT_DEV_API_TOKEN is always set from
    the token file: x4pro-dev reads it via ${sysenv...}, so building with and
    without it changes PlatformIO's config checksum, and PlatformIO then
    deletes every environment's build output."""
    token = TOKEN_FILE.read_text().strip() if TOKEN_FILE.exists() else ""
    if not token:
        sys.exit(f"missing {TOKEN_FILE}")
    env = dict(os.environ, CROSSPOINT_DEV_API_TOKEN=token)
    for t in targets:
        name = BUILD_ENVS[t]
        log = FIRMWARE / f"build-{name}.log"
        print(f"building {name} (log: {log}) ...")
        with open(log, "w") as f:
            rc = subprocess.run([str(PIO), "run", "-e", name], cwd=FIRMWARE, env=env, stdout=f,
                                stderr=subprocess.STDOUT).returncode
        if rc != 0:
            errors = [l for l in log.read_text(errors="replace").splitlines() if "error:" in l][:10]
            sys.exit(f"{name} build failed:\n" + "\n".join(errors))
    print("built")


def cmd_flash(c, args):
    if not args.image:
        build(["device"])
    image = Path(args.image) if args.image else DEVICE_IMAGE
    target = "/.crosspoint/dev-firmware.bin"
    print(f"uploading {image.stat().st_size} bytes ...")
    c.upload(target, image.read_bytes())
    print("flashing ...")
    if isinstance(c, HttpClient):
        c.timeout = 120
    show(c.json("POST", "/dev/flash", {"path": target}))


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", help="device IP for Wi-Fi (default: local simulator)")
    p.add_argument("--serial", metavar="PORT", help="use USB serial: auto, sim, or a port path")
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("sim")
    s.add_argument("action", choices=["start", "stop", "status"])
    s.add_argument("--sd", help="folder to use as the SD card (default ../qa/sim/sd)")
    s.add_argument("--window", action="store_true", help="show the SDL window")
    sub.add_parser("state")
    sub.add_parser("info")
    s = sub.add_parser("screen")
    s.add_argument("-o", "--output", default=str(QA_DIR / "screen.png"))
    s = sub.add_parser("logs")
    s.add_argument("--since", type=int, default=0)
    s.add_argument("--grep")
    s = sub.add_parser("wait")
    s.add_argument("regex")
    s.add_argument("--timeout", type=float, default=60)
    s.add_argument("--since", type=int)
    s = sub.add_parser("open")
    s.add_argument("screen")
    s.add_argument("--path")
    s = sub.add_parser("input")
    s.add_argument("button", choices=["confirm", "back", "next", "previous"])
    s = sub.add_parser("settings")
    s.add_argument("keys", nargs="*")
    s = sub.add_parser("settings-set")
    s.add_argument("pairs", nargs="+", metavar="KEY=VALUE")
    s = sub.add_parser("frontlight")
    s.add_argument("--on", dest="on", action="store_true", default=None)
    s.add_argument("--off", dest="on", action="store_false")
    s.add_argument("--brightness", type=int)
    s.add_argument("--warmth", type=int)
    s = sub.add_parser("power")
    s.add_argument("action", nargs="?", choices=["sleep", "reboot"])
    sub.add_parser("refresh")
    s = sub.add_parser("wifi")
    s.add_argument("--ssid")
    s.add_argument("--password-file")
    s = sub.add_parser("opds-add")
    s.add_argument("--name", required=True)
    s.add_argument("--url", required=True)
    s.add_argument("--user")
    s.add_argument("--password-file")
    s.add_argument("--header-file")
    s = sub.add_parser("fs")
    s.add_argument("op", choices=["ls", "get", "put", "rm", "upload"])
    s.add_argument("path")
    s.add_argument("--file")
    s = sub.add_parser("flash")
    s.add_argument("image", nargs="?")
    s = sub.add_parser("build")
    s.add_argument("target", nargs="?", default="device", choices=["device", "sim", "all"])

    args = p.parse_args()
    if args.cmd == "build":
        build(["device", "sim"] if args.target == "all" else [args.target])
        return
    if args.cmd == "sim":
        {"start": sim_start, "stop": sim_stop,
         "status": lambda a: print("running" if sim_pid() else "stopped")}[args.action](args)
        return
    c = make_client(args)
    try:
        if args.cmd == "state":
            show(c.json("GET", "/dev/state"))
        elif args.cmd == "info":
            show(c.json("GET", "/dev/info"))
        elif args.cmd == "screen":
            print(screenshot(c, Path(args.output)))
        elif args.cmd == "logs":
            cmd_logs(c, args)
        elif args.cmd == "wait":
            print(wait_for_log(c, args.regex, args.since, args.timeout))
        elif args.cmd == "open":
            c.json("POST", "/dev/open", {"screen": args.screen, **({"path": args.path} if args.path else {})})
            print("queued")
        elif args.cmd == "input":
            c.json("POST", "/dev/input", {"button": args.button})
            print("queued")
        elif args.cmd == "settings":
            cmd_settings(c, args)
        elif args.cmd == "settings-set":
            cmd_settings_set(c, args)
        elif args.cmd == "frontlight":
            cmd_frontlight(c, args)
        elif args.cmd == "power":
            cmd_power(c, args)
        elif args.cmd == "refresh":
            show(c.json("POST", "/dev/display", {"refresh": "full"}))
        elif args.cmd == "wifi":
            cmd_wifi(c, args)
        elif args.cmd == "opds-add":
            cmd_opds_add(c, args)
        elif args.cmd == "fs":
            cmd_fs(c, args)
        elif args.cmd == "flash":
            cmd_flash(c, args)
    except ApiError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
