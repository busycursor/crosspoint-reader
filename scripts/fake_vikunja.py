#!/usr/bin/env python3
"""A small fake Vikunja API (v0.24 shape) for simulator tests.

  python3 scripts/fake_vikunja.py --port 18456 --token TOKEN [--require-header NAME=VALUE]
      [--fixture scripts/vikunja_fixtures/home.json] [--per-page-max N] [--down]

Serves GET /api/v1/info, GET /api/v1/projects, GET /api/v1/projects/<id>/views/<view>/tasks
(the list view shows open tasks only, as Vikunja's default list filter does; the table view
honours filter=done = true and sort_by=done_at&order_by=desc), GET /api/v1/tasks/<id> and
POST /api/v1/tasks/<id>. An update replaces the whole task, as Vikunja does, so a body
without "title" blanks it.

A missing --require-header gets 302 to another host, like Cloudflare Access. A wrong token
gets 401, an unknown task 404. --down delays every reply by 10 s (then answers normally).

Test controls (no auth): GET /_state, POST /_delete_task/<id>, POST /_delete_project/<id>,
POST /_set_done/<id>/<0|1>, POST /_star/<project>/<0|1>, POST /_refuse/<task> (its
updates get 403), POST /_add_done/<project>/<n> (n old done items).
"""

from __future__ import annotations

import argparse
import copy
import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

DEFAULT_FIXTURE = Path(__file__).resolve().parent / "vikunja_fixtures" / "home.json"
KINDS = ["list", "gantt", "table", "kanban"]


def view_id(project_id: int, kind: str) -> int:
    return project_id * 10 + KINDS.index(kind) + 1


class FakeVikunja:
    def __init__(self, fixture: dict, token: str, required: tuple[str, str] | None = None,
                 down: bool = False, per_page_max: int = 50):
        self.token = token
        self.required = required
        self.down = down
        self.per_page_max = per_page_max
        self.lock = threading.Lock()
        self.projects = [copy.deepcopy(p) for p in fixture.get("projects", [])]
        self.tasks = {t["id"]: self._full_task(t) for t in fixture.get("tasks", [])}
        self.updates: list[dict] = []
        self.refused: set[int] = set()  # tasks whose update gets 403, like a read-only share

    @staticmethod
    def _full_task(t: dict) -> dict:
        full = {"id": 0, "title": "", "description": "", "done": False, "done_at": "0001-01-01T00:00:00Z",
                "priority": 0, "labels": None, "position": 0, "percent_done": 0.0, "project_id": 0,
                "related_tasks": {}}
        full.update(copy.deepcopy(t))
        return full

    def project_json(self, p: dict) -> dict:
        out = copy.deepcopy(p)
        out["views"] = [{"id": view_id(p["id"], k), "project_id": p["id"], "title": k.title(), "view_kind": k,
                         "filter": "done = false" if k == "list" else ""} for k in KINDS]
        return out

    def view_tasks(self, project_id: int, view: int, query: dict) -> list[dict] | None:
        kind = next((k for k in KINDS if view_id(project_id, k) == view), None)
        if kind is None:
            return None
        tasks = [t for t in self.tasks.values() if t["project_id"] == project_id]
        filt = (query.get("filter") or [""])[0].replace(" ", "")
        if kind == "list":
            tasks = [t for t in tasks if not t["done"]]
        if filt == "done=true":
            tasks = [t for t in tasks if t["done"]]
        elif filt == "done=false":
            tasks = [t for t in tasks if not t["done"]]
        if (query.get("sort_by") or [""])[0] == "done_at":
            tasks.sort(key=lambda t: t["done_at"], reverse=(query.get("order_by") or ["asc"])[0] == "desc")
        else:
            tasks.sort(key=lambda t: t["position"])
        return tasks

    def update_task(self, task_id: int, body: dict) -> dict | None:
        if task_id not in self.tasks:
            return None
        old = self.tasks[task_id]
        new = self._full_task({"id": task_id, "project_id": old["project_id"]})
        new.update({k: v for k, v in body.items() if k != "id"})
        if new["done"] and not old["done"]:
            new["done_at"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        if not new["done"]:
            new["done_at"] = "0001-01-01T00:00:00Z"
        self.tasks[task_id] = new
        self.updates.append({"id": task_id, "done": new["done"]})
        return new


def paginate(items: list, query: dict, per_page_max: int) -> list:
    per_page = min(int((query.get("per_page") or ["50"])[0]), per_page_max)
    page = max(int((query.get("page") or ["1"])[0]), 1)
    return items[(page - 1) * per_page: page * per_page]


def make_handler(fake: FakeVikunja):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):
            pass

        def send_json(self, code: int, payload) -> None:
            body = json.dumps(payload).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def gate(self) -> bool:
            if fake.down:
                time.sleep(10)
            if fake.required and self.headers.get(fake.required[0]) != fake.required[1]:
                self.send_response(302)
                self.send_header("Location", "http://127.0.0.2:9/cdn-cgi/access/login")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return False
            return True

        def authed(self) -> bool:
            if self.headers.get("Authorization") != f"Bearer {fake.token}":
                self.send_json(401, {"code": 11, "message": "invalid token"})
                return False
            return True

        def control(self, path: str) -> bool:
            if path == "/_state":
                with fake.lock:
                    self.send_json(200, {"tasks": list(fake.tasks.values()), "updates": fake.updates,
                                         "projects": fake.projects})
                return True
            m = re.fullmatch(r"/_delete_task/(\d+)", path)
            if m:
                with fake.lock:
                    fake.tasks.pop(int(m.group(1)), None)
                self.send_json(200, {})
                return True
            m = re.fullmatch(r"/_delete_project/(\d+)", path)
            if m:
                with fake.lock:
                    fake.projects = [p for p in fake.projects if p["id"] != int(m.group(1))]
                self.send_json(200, {})
                return True
            m = re.fullmatch(r"/_refuse/(\d+)", path)
            if m:
                with fake.lock:
                    fake.refused.add(int(m.group(1)))
                self.send_json(200, {})
                return True
            m = re.fullmatch(r"/_add_done/(\d+)/(\d+)", path)
            if m:
                with fake.lock:
                    pid, n = int(m.group(1)), int(m.group(2))
                    base = max(fake.tasks) + 1
                    for i in range(n):
                        fake.tasks[base + i] = fake._full_task(
                            {"id": base + i, "project_id": pid, "title": f"Old item {i + 1}", "done": True,
                             "done_at": f"2024-01-01T00:{(n - i) // 60 % 60:02d}:{(n - i) % 60:02d}Z", "position": 1000 + i})
                self.send_json(200, {})
                return True
            m = re.fullmatch(r"/_star/(\d+)/([01])", path)
            if m:
                with fake.lock:
                    for p in fake.projects:
                        if p["id"] == int(m.group(1)):
                            p["is_favorite"] = m.group(2) == "1"
                self.send_json(200, {})
                return True
            m = re.fullmatch(r"/_set_done/(\d+)/([01])", path)
            if m:
                with fake.lock:
                    t = fake.tasks.get(int(m.group(1)))
                    if t:
                        t["done"] = m.group(2) == "1"
                self.send_json(200, {})
                return True
            return False

        def do_GET(self):
            url = urlsplit(self.path)
            query = parse_qs(url.query)
            if self.control(url.path):
                return
            if not self.gate():
                return
            if url.path == "/api/v1/info":
                self.send_json(200, {"version": "v0.24.0"})
                return
            if not self.authed():
                return
            with fake.lock:
                if url.path == "/api/v1/projects":
                    items = [fake.project_json(p) for p in fake.projects]
                    self.send_json(200, paginate(items, query, fake.per_page_max))
                    return
                m = re.fullmatch(r"/api/v1/projects/(\d+)/views/(\d+)/tasks", url.path)
                if m:
                    tasks = fake.view_tasks(int(m.group(1)), int(m.group(2)), query)
                    if tasks is None:
                        self.send_json(404, {"message": "view not found"})
                    else:
                        self.send_json(200, paginate(tasks, query, fake.per_page_max))
                    return
                m = re.fullmatch(r"/api/v1/tasks/(\d+)", url.path)
                if m:
                    t = fake.tasks.get(int(m.group(1)))
                    self.send_json(200, t) if t else self.send_json(404, {"message": "task not found"})
                    return
            self.send_json(404, {"message": "not found"})

        def do_POST(self):
            url = urlsplit(self.path)
            if self.control(url.path):
                return
            if not self.gate() or not self.authed():
                return
            length = int(self.headers.get("Content-Length") or 0)
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
            except json.JSONDecodeError:
                self.send_json(400, {"message": "bad json"})
                return
            m = re.fullmatch(r"/api/v1/tasks/(\d+)", url.path)
            if not m:
                self.send_json(404, {"message": "not found"})
                return
            if int(m.group(1)) in fake.refused:
                self.send_json(403, {"message": "forbidden"})
                return
            with fake.lock:
                t = fake.update_task(int(m.group(1)), body)
            self.send_json(200, t) if t else self.send_json(404, {"message": "task not found"})

    return Handler


def serve(fake: FakeVikunja, port: int) -> ThreadingHTTPServer:
    return ThreadingHTTPServer(("127.0.0.1", port), make_handler(fake))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=18456)
    ap.add_argument("--token", required=True)
    ap.add_argument("--require-header", help="NAME=VALUE that every API request must carry")
    ap.add_argument("--fixture", type=Path, default=DEFAULT_FIXTURE)
    ap.add_argument("--per-page-max", type=int, default=50)
    ap.add_argument("--down", action="store_true")
    args = ap.parse_args()
    required = tuple(args.require_header.split("=", 1)) if args.require_header else None
    fake = FakeVikunja(json.loads(args.fixture.read_text()), args.token, required, args.down, args.per_page_max)
    server = serve(fake, args.port)
    print(f"fake Vikunja on http://127.0.0.1:{args.port}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
