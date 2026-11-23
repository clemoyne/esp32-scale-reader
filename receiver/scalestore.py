#!/usr/bin/env python3
"""Store and page for the bathroom scale's readings.

Standard library only (Python 3.7+). The ESP32 reader POSTs JSON like
    {"kg": 80.00, "lb": 176.4, "time": 1795400000, "rssi": -65, "reader": "scale-reader"}
to /scale. Readings go in SQLite; a repeat of the same time and weight (the
reader retrying after a lost reply) is ignored.

    GET  /scale/                 the page (www/index.html next to this file)
    GET  /scale/api/readings     ?days=N (0 or absent: all), oldest first, each with its person
    GET  /scale/api/status       count, latest reading per person, last post
    POST /scale/api/hide         {"id": 3, "hidden": true}  (false brings it back)
    POST /scale  or  /scale/api/ingest   a reading from the ESP32

Each reading belongs to whoever's weight range it falls in, worked out when
it's read (so changing a range re-sorts every past reading). The ranges are in
--people (JSON, re-read on every request; see people.example.json):
    {"people": [{"name": "A", "min_lb": 170}, {"name": "B", "min_lb": 100, "max_lb": 170},
                {"name": "C", "max_lb": 100}]}
min_lb is inclusive, max_lb exclusive, compared at the 0.1 lb the page shows.
Names stay out of the code and the repository.

    python3 scalestore.py [--port 8091] [--db scale.db] [--people people.json]
    python3 scalestore.py import FILE.jsonl [--db ...]   (one JSON reading per line)
"""
import argparse
import json
import os
import sqlite3
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "www", "index.html")
LB_PER_KG = 2.2046226218
MIN_KG, MAX_KG = 1.0, 300.0
LOCK = threading.Lock()

SCHEMA = """
CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY,
    time INTEGER NOT NULL,          -- when the weight locked, from the reader's clock
    received INTEGER NOT NULL,      -- when it reached the store
    kg REAL NOT NULL,
    rssi INTEGER,
    reader TEXT,
    hidden INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX IF NOT EXISTS readings_time_kg ON readings (time, kg);
"""


def load_people(path):
    """[(name, min_lb or None, max_lb or None)], or [] if the file is missing or wrong."""
    try:
        with open(path) as f:
            people = json.load(f)["people"]
        out = []
        for p in people:
            lo, hi = p.get("min_lb"), p.get("max_lb")
            if not isinstance(p.get("name"), str) or not all(v is None or isinstance(v, (int, float)) for v in (lo, hi)):
                raise ValueError(p)
            out.append((p["name"][:40], lo, hi))
        return out
    except (OSError, ValueError, KeyError, TypeError):
        return []


def person_for(people, kg):
    lb = round(kg * LB_PER_KG, 1)
    for name, lo, hi in people:
        if (lo is None or lb >= lo) and (hi is None or lb < hi):
            return name
    return None


def connect(path):
    db = sqlite3.connect(path, timeout=10)
    db.executescript(SCHEMA)
    return db


def add_reading(db, data, now):
    """Returns (stored, error). stored is False for a repeat."""
    try:
        kg = round(float(data["kg"]), 2)
    except (KeyError, TypeError, ValueError):
        return False, "expected JSON with kg"
    if not MIN_KG <= kg <= MAX_KG:
        return False, "kg out of range"
    t = data.get("time")
    # The reader sends 0 if its clock wasn't set yet; then use arrival time.
    if not isinstance(t, int) or not 1700000000 < t <= now + 300:
        t = now
    received = data.get("received") if isinstance(data.get("received"), int) else now
    rssi = data.get("rssi") if isinstance(data.get("rssi"), int) else None
    reader = str(data.get("reader", ""))[:40]
    with LOCK:
        cur = db.execute("INSERT OR IGNORE INTO readings (time, received, kg, rssi, reader) VALUES (?, ?, ?, ?, ?)",
                         (t, received, kg, rssi, reader))
        db.commit()
    return cur.rowcount == 1, None


def readings(db, days, people):
    q = "SELECT id, time, kg, rssi, hidden FROM readings"
    args = ()
    if days > 0:
        q += " WHERE time >= ?"
        args = (int(time.time()) - days * 86400,)
    rows = db.execute(q + " ORDER BY time", args).fetchall()
    return [{"id": i, "t": t, "kg": kg, "rssi": rssi, "hidden": bool(h), "person": person_for(people, kg)}
            for i, t, kg, rssi, h in rows]


def status(db, people):
    count, hidden, since = db.execute("SELECT COUNT(*), COALESCE(SUM(hidden), 0), MIN(time) FROM readings").fetchone()
    posted = db.execute("SELECT MAX(received) FROM readings").fetchone()[0]
    last = {}
    for t, kg in db.execute("SELECT time, kg FROM readings WHERE hidden = 0 ORDER BY time DESC"):
        who = person_for(people, kg)
        if who not in last:
            last[who] = {"t": t, "kg": kg}
        if len(last) > len(people):
            break
    return {"now": int(time.time()), "count": count, "hidden": hidden, "since": since, "last_received": posted,
            "people": people_json(people),
            "last": [dict(person=k, **v) for k, v in last.items()]}


def people_json(people):
    return [{"name": n, "min_lb": lo, "max_lb": hi} for n, lo, hi in people]


def make_handler(db_path, people_path):
    local = threading.local()

    def db():
        if not hasattr(local, "db"):
            local.db = connect(db_path)
        return local.db

    class Handler(BaseHTTPRequestHandler):
        def _json(self, code, obj):
            body = json.dumps(obj).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            return json.loads(self.rfile.read(min(n, 4096)).decode() or "null")

        def do_GET(self):
            path, _, query = self.path.partition("?")
            params = dict(p.partition("=")[::2] for p in query.split("&") if p)
            if path == "/scale":
                self.send_response(302)
                self.send_header("Location", "/scale/")
                self.end_headers()
            elif path == "/scale/":
                try:
                    with open(PAGE, "rb") as f:
                        body = f.read()
                except OSError:
                    return self._json(500, {"error": "page missing"})
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                self.wfile.write(body)
            elif path == "/scale/api/readings":
                try:
                    days = max(0, int(params.get("days") or 0))
                except ValueError:
                    return self._json(400, {"error": "days must be a number"})
                people = load_people(people_path)
                self._json(200, {"now": int(time.time()), "people": people_json(people),
                                 "readings": readings(db(), days, people)})
            elif path == "/scale/api/status":
                self._json(200, status(db(), load_people(people_path)))
            else:
                self._json(404, {"error": "not found"})

        def do_POST(self):
            path = self.path.partition("?")[0]
            try:
                data = self._body()
            except ValueError:
                return self._json(400, {"error": "expected JSON"})
            if not isinstance(data, dict):
                return self._json(400, {"error": "expected a JSON object"})
            if path in ("/scale", "/scale/api/ingest"):
                stored, err = add_reading(db(), data, int(time.time()))
                if err:
                    return self._json(400, {"error": err})
                if stored:
                    log("reading %.2f kg = %.1f lb (rssi %s)" % (float(data["kg"]), float(data["kg"]) * LB_PER_KG,
                                                                  data.get("rssi")))
                self._json(200, {"ok": True, "stored": stored})
            elif path == "/scale/api/hide":
                rid, hidden = data.get("id"), data.get("hidden")
                if not isinstance(rid, int) or not isinstance(hidden, bool):
                    return self._json(400, {"error": "expected id and hidden"})
                with LOCK:
                    n = db().execute("UPDATE readings SET hidden = ? WHERE id = ?", (int(hidden), rid)).rowcount
                    db().commit()
                if not n:
                    return self._json(404, {"error": "no such reading"})
                log("reading %d %s" % (rid, "hidden" if hidden else "shown again"))
                self._json(200, {"ok": True})
            else:
                self._json(404, {"error": "not found"})

        def log_message(self, fmt, *args):
            pass

    return Handler


def log(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    sys.stdout.flush()


def import_jsonl(db_path, path):
    db = connect(db_path)
    added = 0
    with open(path) as f:
        for line in f:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            stored, _ = add_reading(db, rec, int(rec.get("received") or time.time()))
            added += stored
    print("imported %d new reading(s) from %s" % (added, path))


def main():
    ap = argparse.ArgumentParser(description="Store and page for the bathroom scale's readings.")
    ap.add_argument("command", nargs="?", choices=["serve", "import"], default="serve")
    ap.add_argument("file", nargs="?", help="for import: one JSON reading per line")
    ap.add_argument("--port", type=int, default=8091)
    ap.add_argument("--db", default="scale.db")
    ap.add_argument("--people", default="people.json", help="who each weight range belongs to")
    a = ap.parse_args()
    if a.command == "import":
        if not a.file:
            ap.error("import needs a file")
        return import_jsonl(a.db, a.file)
    connect(a.db).close()
    srv = ThreadingHTTPServer(("0.0.0.0", a.port), make_handler(a.db, a.people))
    people = load_people(a.people)
    log("listening on port %d, database %s, %d people from %s" % (a.port, a.db, len(people), a.people))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
