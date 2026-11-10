#!/usr/bin/env python3
"""A small fake Home Assistant REST API for simulator tests.

  python3 scripts/fake_ha.py --port 18123 --token TOKEN [--fixture scripts/ha_fixtures/home.json] [--down] [--lag S]

Serves GET /api/states, GET and POST (set) /api/states/<id>, POST /api/services/<domain>/<service>
(toggle, turn_on/turn_off with brightness_pct, scene/script turn_on, button press),
POST /api/services/weather/get_forecasts?return_response and GET /api/lovelace-fixture
(the fixture's dashboard, for the simulator's REST session). A wrong token gets 401,
an unknown entity 404. --down makes every request hang for 10 s.
"""

from __future__ import annotations

import argparse
import copy
import json
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

DEFAULT_FIXTURE = Path(__file__).resolve().parent / "ha_fixtures" / "home.json"


class FakeHa:
    def __init__(self, fixture: dict, token: str, down: bool = False, lag: float = 0.0):
        self.token = token
        self.down = down
        self.lag = lag  # seconds before a service call's state change lands, like a slow device
        self.lock = threading.Lock()
        self.states = {s["entity_id"]: copy.deepcopy(s) for s in fixture.get("states", [])}
        self.forecast = fixture.get("forecast", [])
        self.dashboard = fixture.get("dashboard", {"views": []})
        self.calls: list[tuple[str, str, dict]] = []

    def call_service(self, domain: str, service: str, data: dict) -> bool:
        if self.lag > 0:
            with self.lock:
                known = data.get("entity_id", "") in self.states
            if known:
                threading.Timer(self.lag, self._apply, (domain, service, data)).start()
            return known
        return self._apply(domain, service, data)

    def _apply(self, domain: str, service: str, data: dict) -> bool:
        entity = data.get("entity_id", "")
        with self.lock:
            self.calls.append((domain, service, data))
            s = self.states.get(entity)
            if s is None:
                return False
            attrs = s.setdefault("attributes", {})
            if service == "toggle":
                service = "turn_off" if s["state"] == "on" else "turn_on"
            if service == "turn_on":
                if domain in ("scene", "script"):
                    s["state"] = time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())
                else:
                    s["state"] = "on"
                    if domain == "light":
                        pct = data.get("brightness_pct")
                        attrs["brightness"] = round(int(pct) * 255 / 100) if pct is not None else attrs.get("brightness") or 255
            elif service == "turn_off":
                s["state"] = "off"
                if domain == "light":
                    attrs["brightness"] = None
            elif service == "press":
                s["state"] = time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())
            return True

    def set_state(self, entity: str, state: str) -> None:
        with self.lock:
            self.states[entity]["state"] = state


def make_handler(ha: FakeHa):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *args):  # quiet
            pass

        def _send(self, code: int, body) -> None:
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def _authorized(self) -> bool:
            if ha.down:
                time.sleep(10)
            if self.headers.get("Authorization") != f"Bearer {ha.token}":
                self._send(401, {"message": "Unauthorized"})
                return False
            return True

        def do_GET(self):
            if not self._authorized():
                return
            path = urlsplit(self.path).path
            if path == "/api/":
                return self._send(200, {"message": "API running."})
            if path == "/api/states":
                with ha.lock:
                    return self._send(200, list(ha.states.values()))
            if path.startswith("/api/states/"):
                with ha.lock:
                    s = ha.states.get(path[len("/api/states/"):])
                    return self._send(200, s) if s else self._send(404, {"message": "Entity not found."})
            if path == "/api/lovelace-fixture":
                return self._send(200, ha.dashboard)
            self._send(404, {"message": "Not found"})

        def do_POST(self):
            if not self._authorized():
                return
            url = urlsplit(self.path)
            parts = url.path.split("/")
            length = int(self.headers.get("Content-Length") or 0)
            data = json.loads(self.rfile.read(length) or b"{}")
            if len(parts) == 4 and parts[1:3] == ["api", "states"]:  # set a state, as HA's REST API does
                with ha.lock:
                    s = ha.states.setdefault(parts[3], {"entity_id": parts[3], "attributes": {}})
                    s["state"] = str(data.get("state", ""))
                    s["attributes"].update(data.get("attributes", {}))
                    return self._send(200, s)
            if len(parts) == 5 and parts[1:3] == ["api", "services"]:
                domain, service = parts[3], parts[4]
                if domain == "weather" and service == "get_forecasts":
                    entity = data.get("entity_id", "")
                    return self._send(200, {"changed_states": [], "service_response": {entity: {"forecast": ha.forecast}}})
                if not ha.call_service(domain, service, data):
                    return self._send(400, {"message": "Entity not found"})
                with ha.lock:
                    s = ha.states.get(data.get("entity_id", ""))
                return self._send(200, [s] if s else [])
            self._send(404, {"message": "Not found"})

    return Handler


def serve(ha: FakeHa, port: int) -> ThreadingHTTPServer:
    server = ThreadingHTTPServer(("127.0.0.1", port), make_handler(ha))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=18123)
    ap.add_argument("--token", required=True)
    ap.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    ap.add_argument("--down", action="store_true")
    ap.add_argument("--lag", type=float, default=0.0, help="seconds before a service call's state change lands")
    args = ap.parse_args()
    ha = FakeHa(json.loads(args.fixture.read_text()), args.token, args.down, args.lag)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(ha))
    print(f"fake HA on http://127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
