"""Run with python3 scripts/test_fake_vikunja.py."""

import json
import socket
import sys
import threading
import unittest
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fake_vikunja  # noqa: E402

HEADER = ("CF-Access-Client-Id", "test")


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


class FakeVikunjaTest(unittest.TestCase):
    def setUp(self):
        fixture = json.loads(fake_vikunja.DEFAULT_FIXTURE.read_text())
        self.fake = fake_vikunja.FakeVikunja(fixture, "tok", HEADER, per_page_max=2)
        self.port = free_port()
        self.server = fake_vikunja.serve(self.fake, self.port)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()

    def call(self, path, body=None, token="tok", header=True):
        req = urllib.request.Request(f"http://127.0.0.1:{self.port}{path}",
                                     data=json.dumps(body).encode() if body is not None else None,
                                     method="POST" if body is not None else "GET")
        if token:
            req.add_header("Authorization", f"Bearer {token}")
        if header:
            req.add_header(*HEADER)
        try:
            with OPENER.open(req) as r:
                return r.status, json.loads(r.read() or b"null")
        except urllib.error.HTTPError as e:
            return e.code, None

    def test_projects_paginate(self):
        _, p1 = self.call("/api/v1/projects?per_page=50&page=1")
        _, p2 = self.call("/api/v1/projects?per_page=50&page=2")
        _, p3 = self.call("/api/v1/projects?per_page=50&page=3")
        self.assertEqual(len(p1), 2)
        self.assertEqual(len(p2), 2)
        self.assertEqual(p3, [])
        self.assertEqual([v["view_kind"] for v in p1[0]["views"]], fake_vikunja.KINDS)

    def test_list_view_hides_done(self):
        list_view = fake_vikunja.view_id(7, "list")
        _, page = self.call(f"/api/v1/projects/7/views/{list_view}/tasks?per_page=50&page=1")
        self.assertEqual([t["title"] for t in page], ["Milk", "Bread"])
        _, done = self.call(f"/api/v1/projects/7/views/{list_view}/tasks?filter=done%20%3D%20true&page=1")
        self.assertTrue(all(not t["done"] for t in done))

    def test_table_view_done_newest_first(self):
        table = fake_vikunja.view_id(7, "table")
        _, page = self.call(f"/api/v1/projects/7/views/{table}/tasks?filter=done%20%3D%20true"
                            "&sort_by=done_at&order_by=desc&per_page=50&page=1")
        self.assertEqual([t["title"] for t in page], ["Coffee", "Apples"])

    def test_bad_token_401(self):
        self.assertEqual(self.call("/api/v1/projects", token="nope")[0], 401)

    def test_missing_header_302(self):
        self.assertEqual(self.call("/api/v1/projects", header=False)[0], 302)

    def test_update_replaces_whole_task(self):
        status, task = self.call("/api/v1/tasks/101", body={"done": True})
        self.assertEqual(status, 200)
        self.assertTrue(task["done"])
        self.assertEqual(task["title"], "")

    def test_full_update_keeps_title(self):
        _, task = self.call("/api/v1/tasks/101")
        task["done"] = True
        _, updated = self.call("/api/v1/tasks/101", body=task)
        self.assertEqual(updated["title"], "Milk")
        self.assertNotEqual(updated["done_at"], "0001-01-01T00:00:00Z")

    def test_unknown_task_404(self):
        self.assertEqual(self.call("/api/v1/tasks/999")[0], 404)
        self.call("/_delete_task/101", body={})
        self.assertEqual(self.call("/api/v1/tasks/101")[0], 404)


if __name__ == "__main__":
    unittest.main()
