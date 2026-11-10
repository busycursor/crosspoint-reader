#!/usr/bin/env python3
"""MCP server exposing the firmware dev control API (see devctl.py).

Run with:  uv run --with 'mcp<2' python scripts/devctl_mcp.py

Target (environment):
  default              the local simulator over HTTP
  XTEINK_HOST=<ip>     a device over Wi-Fi
  XTEINK_SERIAL=<path> a device over USB serial ("auto" = first /dev/cu.usbmodem*, "sim" = simulator pty)
Each tool also takes an optional `target` overriding that: "sim", "serial", "serial:<path>" or an IP.
"""

from __future__ import annotations

import os
import re
import sys
from pathlib import Path
from types import SimpleNamespace

from mcp.server.fastmcp import FastMCP, Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import devctl  # noqa: E402

mcp = FastMCP("xteink")
_serial_clients: dict[str, devctl.SerialClient] = {}  # one open port per path, reused across calls


def client(target: str | None = None) -> devctl.Client:
    target = target or ("serial:" + os.environ["XTEINK_SERIAL"] if os.environ.get("XTEINK_SERIAL")
                        else os.environ.get("XTEINK_HOST") or "sim")
    if target == "sim":
        return devctl.make_client(SimpleNamespace(host=None, serial=None))
    if target.startswith("serial"):
        path = target.partition(":")[2]
        key = path or "auto"
        if key not in _serial_clients:
            _serial_clients[key] = devctl.make_client(
                SimpleNamespace(host=None, serial="" if path in ("", "auto") else path))
        return _serial_clients[key]
    return devctl.make_client(SimpleNamespace(host=target, serial=None))


# --- simulator --------------------------------------------------------------------------

@mcp.tool()
def sim_start(window: bool = False) -> str:
    """Start the headless X4 Pro simulator (build first: pio run -e simulator_x4_pro)."""
    devctl.sim_start(SimpleNamespace(sd=None, window=window))
    return "started"


@mcp.tool()
def sim_stop() -> str:
    """Stop the simulator."""
    devctl.sim_stop()
    return "stopped"


# --- observe ----------------------------------------------------------------------------

@mcp.tool()
def state(target: str | None = None) -> dict:
    """Current activity, on-screen text, heap, Wi-Fi and the current log sequence number."""
    return client(target).json("GET", "/dev/state")


@mcp.tool()
def info(target: str | None = None) -> dict:
    """Firmware version, board, display size, SD card usage."""
    return client(target).json("GET", "/dev/info")


@mcp.tool()
def screenshot(target: str | None = None) -> Image:
    """The current screen as a PNG, in the orientation the user sees."""
    out = devctl.QA_DIR / "mcp-screen.png"
    devctl.screenshot(client(target), out)
    return Image(path=str(out))


@mcp.tool()
def logs(since: int = 0, grep: str | None = None, target: str | None = None) -> dict:
    """Log lines with sequence number >= since, optionally filtered by regex. Returns lines and next."""
    doc = client(target).json("GET", "/dev/logs", query={"since": since})
    if grep:
        pat = re.compile(grep)
        doc["lines"] = [l for l in doc["lines"] if pat.search(l)]
    return doc


@mcp.tool()
def wait_for_log(regex: str, since: int | None = None, timeout_s: float = 60, target: str | None = None) -> str:
    """Block until a log line matches regex (tolerates the firmware being busy)."""
    return devctl.wait_for_log(client(target), regex, since, timeout_s)


# --- drive ------------------------------------------------------------------------------

@mcp.tool()
def open_screen(screen: str, path: str | None = None, target: str | None = None) -> str:
    """Go to a screen: home, sync, settings, library, files, transfer, control, opds, reader (path = book on SD)."""
    client(target).json("POST", "/dev/open", {"screen": screen, **({"path": path} if path else {})})
    return "queued"


@mcp.tool()
def press(button: str, target: str | None = None) -> str:
    """Inject one click: confirm, back, next, previous."""
    client(target).json("POST", "/dev/input", {"button": button})
    return "queued"


@mcp.tool()
def sync(timeout_s: float = 300, target: str | None = None) -> str:
    """Run the shelf sync and return its RESULT log line."""
    c = client(target)
    since = c.json("GET", "/dev/state")["log_seq"]
    c.json("POST", "/dev/open", {"screen": "sync"})
    return devctl.wait_for_log(c, r"SYNC.*RESULT", since, timeout_s)


# --- device features --------------------------------------------------------------------

@mcp.tool()
def settings(keys: list[str] | None = None, target: str | None = None) -> list:
    """All settings (or just `keys`) with value, type, options or range: the same data as the web settings page."""
    query = {"keys": ",".join(keys)} if keys else None
    return client(target).json("GET", "/dev/settings", query=query)


@mcp.tool()
def set_settings(values: dict, target: str | None = None) -> dict:
    """Apply settings by key (enum values are option indexes) and save. Returns applied and rejected keys."""
    return client(target).json("POST", "/dev/settings", values)


@mcp.tool()
def frontlight(on: bool | None = None, brightness: int | None = None, warmth: int | None = None,
               target: str | None = None) -> dict:
    """Read or set the frontlight (brightness and warmth 0-100; warmth 0 = cool). Saved to settings."""
    payload = {k: v for k, v in {"on": on, "brightness": brightness, "warmth": warmth}.items() if v is not None}
    c = client(target)
    return c.json("POST", "/dev/frontlight", payload) if payload else c.json("GET", "/dev/frontlight")


@mcp.tool()
def power(action: str | None = None, target: str | None = None) -> dict:
    """Battery and USB state; action "sleep" or "reboot" acts. Sleep turns the API off until the power button."""
    c = client(target)
    return c.json("POST", "/dev/power", {"action": action}) if action else c.json("GET", "/dev/power")


@mcp.tool()
def full_refresh(target: str | None = None) -> dict:
    """Full e-ink refresh of the current screen (clears ghosting)."""
    return client(target).json("POST", "/dev/display", {"refresh": "full"})


@mcp.tool()
def wifi(ssid: str | None = None, password_file: str | None = None, target: str | None = None) -> dict:
    """Wi-Fi status, or save a network and connect (password read from a local file)."""
    c = client(target)
    if not ssid:
        return c.json("GET", "/dev/wifi")
    password = Path(password_file).read_text().strip() if password_file else ""
    return c.json("POST", "/dev/wifi", {"ssid": ssid, "password": password})


@mcp.tool()
def build(which: str = "device") -> str:
    """Build firmware: "device" (x4pro), "sim" (simulator) or "all"."""
    devctl.build(["device", "sim"] if which == "all" else [which])
    return "built"


@mcp.tool()
def flash(image_path: str | None = None, target: str | None = None) -> dict:
    """Flash the device and reboot it: uploads to the SD card, then flashes. Without image_path,
    builds the device firmware first. USB/esptool stays the recovery path."""
    c = client(target)
    devctl.cmd_flash(c, SimpleNamespace(image=image_path))
    return {"status": "flashed; rebooting"}


# --- files and servers ------------------------------------------------------------------

@mcp.tool()
def fs_list(path: str = "/", target: str | None = None) -> list:
    """List an SD card directory."""
    return client(target).json("GET", "/dev/fs", query={"path": path})["entries"]


@mcp.tool()
def fs_read(path: str, max_bytes: int = 65536, target: str | None = None) -> str:
    """Read a text file from the SD card."""
    code, _, data = client(target).request("GET", "/dev/fs", query={"path": path})
    if code != 200:
        raise devctl.ApiError(f"read {path}: HTTP {code}")
    return data[:max_bytes].decode(errors="replace")


@mcp.tool()
def fs_write(path: str, content: str, target: str | None = None) -> str:
    """Write a text file to the SD card (creates parent folders)."""
    code, _, resp = client(target).request("PUT", "/dev/fs", query={"path": path}, body=content.encode(),
                                           content_type="text/plain")
    if code >= 400:
        raise devctl.ApiError(f"write {path}: HTTP {code} {resp!r}")
    return "written"


@mcp.tool()
def fs_upload(local_path: str, device_path: str, target: str | None = None) -> str:
    """Upload any local file (e.g. an EPUB or font) to the SD card."""
    client(target).upload(device_path, Path(local_path).read_bytes())
    return "uploaded"


@mcp.tool()
def fs_delete(path: str, target: str | None = None) -> str:
    """Delete a file or folder on the SD card."""
    client(target).json("DELETE", "/dev/fs", query={"path": path})
    return "deleted"


@mcp.tool()
def opds_servers(target: str | None = None) -> dict:
    """Configured OPDS servers (secrets are not returned)."""
    return client(target).json("GET", "/dev/opds")


if __name__ == "__main__":
    mcp.run()
