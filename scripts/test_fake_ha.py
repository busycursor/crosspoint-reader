"""Run with python3 scripts/test_fake_ha.py."""

import json
import socket
import sys
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fake_ha  # noqa: E402


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class FakeHaTest(unittest.TestCase):
    def setUp(self):
        self.ha = fake_ha.FakeHa(json.loads(fake_ha.DEFAULT_FIXTURE.read_text()), "tok")
        self.port = free_port()
        self.server = fake_ha.serve(self.ha, self.port)

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def request(self, method, path, body=None, token="tok"):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}", method=method,
                                     data=json.dumps(body).encode() if body is not None else None,
                                     headers={"Authorization": f"Bearer {token}", "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=5) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_toggle_flips_state(self):
        self.assertEqual(self.request("POST", "/api/services/light/toggle", {"entity_id": "light.desk_lamp"})[0], 200)
        self.assertEqual(self.request("GET", "/api/states/light.desk_lamp")[1]["state"], "on")
        self.request("POST", "/api/services/light/toggle", {"entity_id": "light.desk_lamp"})
        self.assertEqual(self.request("GET", "/api/states/light.desk_lamp")[1]["state"], "off")

    def test_brightness_pct_sets_brightness(self):
        self.request("POST", "/api/services/light/turn_on", {"entity_id": "light.desk_lamp", "brightness_pct": 40})
        s = self.request("GET", "/api/states/light.desk_lamp")[1]
        self.assertEqual(s["state"], "on")
        self.assertEqual(s["attributes"]["brightness"], 102)

    def test_set_state(self):
        self.request("POST", "/api/states/sensor.timer_remaining", {"state": "0:10:00"})
        self.assertEqual(self.request("GET", "/api/states/sensor.timer_remaining")[1]["state"], "0:10:00")

    def test_wrong_token_is_401(self):
        self.assertEqual(self.request("GET", "/api/states/light.desk_lamp", token="nope")[0], 401)

    def test_unknown_entity_is_404(self):
        self.assertEqual(self.request("GET", "/api/states/light.nope")[0], 404)

    def test_forecast_and_dashboard(self):
        code, body = self.request("POST", "/api/services/weather/get_forecasts?return_response",
                                  {"entity_id": "weather.home", "type": "daily"})
        self.assertEqual(code, 200)
        self.assertEqual(len(body["service_response"]["weather.home"]["forecast"]), 3)
        self.assertEqual(self.request("GET", "/api/lovelace-fixture")[1]["title"], "E-reader")


if __name__ == "__main__":
    unittest.main()
