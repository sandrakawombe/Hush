"""SQLite catalog of test calls, their labeled events, and every scoring run.

One file (bench/data/catalog.db), no server. Audio stays as plain WAV files;
the database holds what is in them and how each detector scored.
"""
import json
import sqlite3
from datetime import datetime, timezone
from pathlib import Path

DB_PATH = Path(__file__).resolve().parent.parent / "data" / "catalog.db"

SCHEMA = """
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS clips (
  id          INTEGER PRIMARY KEY,
  name        TEXT UNIQUE NOT NULL,
  split       TEXT NOT NULL CHECK (split IN ('dev', 'test')),
  path        TEXT NOT NULL,          -- relative to bench/data
  duration_s  REAL NOT NULL,
  speaker     TEXT NOT NULL,
  noise_db    REAL                    -- background noise level, NULL = quiet room
);

CREATE TABLE IF NOT EXISTS events (
  id          INTEGER PRIMARY KEY,
  clip_id     INTEGER NOT NULL REFERENCES clips(id) ON DELETE CASCADE,
  kind        TEXT NOT NULL,          -- coughing, sneezing, laughing, clapping, ...
  is_target   INTEGER NOT NULL,       -- 1 = should be muted, 0 = must stay audible
  onset_s     REAL NOT NULL,
  offset_s    REAL NOT NULL,
  over_speech INTEGER NOT NULL,       -- 1 = happens while the person is talking
  source      TEXT NOT NULL           -- original file, for licensing and debugging
);

CREATE TABLE IF NOT EXISTS speech (
  id       INTEGER PRIMARY KEY,
  clip_id  INTEGER NOT NULL REFERENCES clips(id) ON DELETE CASCADE,
  start_s  REAL NOT NULL,
  end_s    REAL NOT NULL
);

CREATE TABLE IF NOT EXISTS runs (
  id          INTEGER PRIMARY KEY,
  detector    TEXT NOT NULL,
  split       TEXT NOT NULL,
  created_at  TEXT NOT NULL,
  params      TEXT NOT NULL,          -- JSON
  metrics     TEXT NOT NULL           -- JSON
);

CREATE TABLE IF NOT EXISTS run_events (
  run_id    INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  event_id  INTEGER NOT NULL REFERENCES events(id) ON DELETE CASCADE,
  coverage  REAL NOT NULL,            -- share of the event's attack that was muted
  leak_ms   REAL,                     -- how late the mute started, NULL if never muted
  PRIMARY KEY (run_id, event_id)
);

CREATE TABLE IF NOT EXISTS run_mutes (
  run_id    INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,
  clip_id   INTEGER NOT NULL REFERENCES clips(id) ON DELETE CASCADE,
  start_s   REAL NOT NULL,
  end_s     REAL NOT NULL,
  is_false  INTEGER NOT NULL          -- 1 = did not overlap any cough or sneeze
);

CREATE INDEX IF NOT EXISTS events_clip ON events(clip_id);
CREATE INDEX IF NOT EXISTS speech_clip ON speech(clip_id);
CREATE INDEX IF NOT EXISTS mutes_run ON run_mutes(run_id);
"""


def connect(path=DB_PATH):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    con = sqlite3.connect(path)
    con.row_factory = sqlite3.Row
    con.executescript(SCHEMA)
    return con


def reset_clips(con):
    """Drop all clips (and with them events, speech and old runs' per-clip rows)."""
    con.execute("DELETE FROM runs")
    con.execute("DELETE FROM clips")
    con.commit()


def add_clip(con, name, split, path, duration_s, speaker, noise_db, events, speech):
    cur = con.execute(
        "INSERT INTO clips (name, split, path, duration_s, speaker, noise_db) VALUES (?,?,?,?,?,?)",
        (name, split, path, duration_s, speaker, noise_db),
    )
    cid = cur.lastrowid
    con.executemany(
        "INSERT INTO events (clip_id, kind, is_target, onset_s, offset_s, over_speech, source) VALUES (?,?,?,?,?,?,?)",
        [(cid, e["kind"], int(e["is_target"]), e["onset_s"], e["offset_s"], int(e["over_speech"]), e["source"]) for e in events],
    )
    con.executemany(
        "INSERT INTO speech (clip_id, start_s, end_s) VALUES (?,?,?)",
        [(cid, a, b) for a, b in speech],
    )
    return cid


def clips(con, split):
    return con.execute("SELECT * FROM clips WHERE split = ? ORDER BY id", (split,)).fetchall()


def clip_labels(con, clip_id):
    ev = [dict(r) for r in con.execute("SELECT * FROM events WHERE clip_id = ? ORDER BY onset_s", (clip_id,))]
    sp = [(r["start_s"], r["end_s"]) for r in con.execute("SELECT start_s, end_s FROM speech WHERE clip_id = ?", (clip_id,))]
    return ev, sp


def save_run(con, detector, split, params, metrics, per_event, mutes):
    cur = con.execute(
        "INSERT INTO runs (detector, split, created_at, params, metrics) VALUES (?,?,?,?,?)",
        (detector, split, datetime.now(timezone.utc).isoformat(timespec="seconds"), json.dumps(params), json.dumps(metrics)),
    )
    rid = cur.lastrowid
    con.executemany("INSERT INTO run_events VALUES (?,?,?,?)", [(rid, e, c, l) for e, c, l in per_event])
    con.executemany("INSERT INTO run_mutes VALUES (?,?,?,?,?)", [(rid, c, a, b, f) for c, a, b, f in mutes])
    con.commit()
    return rid
