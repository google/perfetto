# Copyright (C) 2026 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
"""SQLite persistence of query executions and their results."""

import json
import os
import sqlite3
import threading
from typing import Any, Dict, List, Optional, Sequence, Tuple

from perfetto.bigtrace.server.util import iso_now

HISTORY_DB_NAME = ".bigtrace_history.db"

# Execution keys that only mean something while the server is running.
_RUNTIME_KEYS = ("_rows", "_view", "_cancel", "_active", "_claimed_rows")

TERMINAL_STATUSES = ("SUCCESS", "FAILED", "CANCELLED")

STATUS_FIELDS = (
    "queryUuid",
    "status",
    "startTime",
    "endTime",
    "processedRows",
    "processedTraces",
    "totalTraces",
    "error",
    "errorMessage",
)

PUBLIC_FIELDS = STATUS_FIELDS + (
    "perfettoSql",
    "limit",
    "traceLimit",
    "materialized",
    "tableName",
    "tableLink",
    "schema",
    "settings",
    "traceFilters",
    "traceMetadataColumns",
    "traceOrderBy",
    "experimentFilter",
)


def public_view(ex: Dict[str, Any],
                fields: Tuple[str, ...] = PUBLIC_FIELDS) -> Dict[str, Any]:
  """Returns the wire form of an execution.

  Unset fields are omitted, as in proto3 JSON, which is what the UI expects:
  it treats a present-but-empty schema as known and a null error as a string.
  """
  return {k: ex[k] for k in fields if ex.get(k) not in (None, [], {})}


class BigtraceHistoryDb:
  """SQLite store for query executions and their result rows.

  Each execution is a JSON document; `status` and `start_time` are also
  columns, for crash recovery and ordering. Results are stored as the encoded
  batches trace processor returned (see results.py), so saving millions of
  rows is a bulk insert of blobs rather than per-row JSON.
  """

  SCHEMA_VERSION = 3
  # Queries whose result rows are kept; older ones keep only their metadata.
  MAX_RESULTS = 20

  def __init__(self, db_path: Optional[str] = None):
    self.db_path = ":memory:"
    if db_path and db_path != ":memory:":
      self.db_path = os.path.abspath(db_path)
      os.makedirs(os.path.dirname(self.db_path), exist_ok=True)
    self._lock = threading.Lock()
    self._conn = self._connect()
    version = self._conn.execute("PRAGMA user_version").fetchone()[0]
    is_history = self._conn.execute(
        "SELECT 1 FROM sqlite_master WHERE name = 'executions'").fetchone()
    if version != self.SCHEMA_VERSION and is_history:
      # History is only a record of past queries: start over, don't migrate.
      # Deleting the file is instant; dropping GBs of results is not.
      self._conn.close()
      for suffix in ("", "-wal", "-shm"):
        try:
          os.remove(self.db_path + suffix)
        except OSError:
          pass
      self._conn = self._connect()
    self._init_schema()

  def _connect(self) -> sqlite3.Connection:
    conn = sqlite3.connect(self.db_path, check_same_thread=False)
    if self.db_path != ":memory:":
      # Only applies to new files; lets pruned results give space back.
      conn.execute("PRAGMA auto_vacuum=INCREMENTAL")
      conn.execute("PRAGMA journal_mode=WAL")
      conn.execute("PRAGMA synchronous=NORMAL")
      conn.execute(f"PRAGMA journal_size_limit={64 << 20}")
    return conn

  def _init_schema(self) -> None:
    with self._lock:
      with self._conn:
        self._conn.execute(f"PRAGMA user_version = {self.SCHEMA_VERSION}")
        self._conn.execute("""
            CREATE TABLE IF NOT EXISTS executions (
              query_uuid TEXT PRIMARY KEY,
              status TEXT NOT NULL,
              start_time TEXT,
              data TEXT NOT NULL
            )""")
        self._conn.execute("""
            CREATE TABLE IF NOT EXISTS results (
              query_uuid TEXT NOT NULL,
              chunk INTEGER NOT NULL,
              meta TEXT NOT NULL,
              columns INTEGER NOT NULL,
              count INTEGER NOT NULL,
              batch BLOB NOT NULL,
              PRIMARY KEY (query_uuid, chunk)
            )""")
      stale = self._conn.execute(
          "SELECT data FROM executions WHERE status = 'IN_PROGRESS'").fetchall(
          )
    for (data,) in stale:
      ex = json.loads(data)
      ex.update(
          status="CANCELLED",
          endTime=iso_now(),
          errorMessage="Interrupted by server restart")
      self.save_execution(ex)

  def save_execution(self, ex: Dict[str, Any]) -> None:
    """Inserts or replaces an execution, without its runtime state."""
    doc = {k: v for k, v in ex.items() if k not in _RUNTIME_KEYS}
    with self._lock, self._conn:
      self._conn.execute(
          "INSERT OR REPLACE INTO executions VALUES (?, ?, ?, ?)",
          (doc["queryUuid"], doc.get("status", "UNKNOWN"), doc.get("startTime"),
           json.dumps(doc, default=str)))

  def save_results(self, query_uuid: str,
                   records: Sequence[Tuple[str, int, int, bytes]]) -> None:
    """Stores ResultSet.to_records(), replacing earlier results.

    Only the MAX_RESULTS most recent queries keep their rows.
    """
    with self._lock:
      with self._conn:
        self._conn.execute("DELETE FROM results WHERE query_uuid = ?",
                           (query_uuid,))
        self._conn.executemany(
            "INSERT INTO results VALUES (?, ?, ?, ?, ?, ?)",
            ((query_uuid, i, *r) for i, r in enumerate(records)))
        pruned = self._conn.execute(
            """DELETE FROM results WHERE query_uuid NOT IN (
                 SELECT query_uuid FROM executions
                 ORDER BY start_time DESC, rowid DESC LIMIT ?)""",
            (self.MAX_RESULTS,)).rowcount
      if pruned:
        self._conn.execute("PRAGMA incremental_vacuum")

  def get_results(self, query_uuid: str) -> List[Tuple[str, int, int, bytes]]:
    """Returns records for ResultSet.from_records()."""
    with self._lock:
      return self._conn.execute(
          "SELECT meta, columns, count, batch FROM results "
          "WHERE query_uuid = ? ORDER BY chunk", (query_uuid,)).fetchall()

  def get_execution(self, query_uuid: str) -> Optional[Dict[str, Any]]:
    """Returns the stored execution, including private keys."""
    with self._lock:
      row = self._conn.execute(
          "SELECT data FROM executions WHERE query_uuid = ?",
          (query_uuid,)).fetchone()
    return json.loads(row[0]) if row else None

  def list_executions(self) -> List[Dict[str, Any]]:
    """Returns public views of all executions, newest first."""
    with self._lock:
      rows = self._conn.execute(
          "SELECT data FROM executions ORDER BY start_time DESC, rowid DESC"
      ).fetchall()
    return [public_view(json.loads(data)) for (data,) in rows]

  def get_status(self, query_uuid: str) -> Optional[Dict[str, Any]]:
    ex = self.get_execution(query_uuid)
    return public_view(ex, STATUS_FIELDS) if ex else None

  def mark_cancelled(self, query_uuid: str) -> None:
    """Cancels a stored execution unless it already finished."""
    ex = self.get_execution(query_uuid)
    if ex and ex.get("status") == "IN_PROGRESS":
      ex.update(status="CANCELLED", endTime=iso_now())
      self.save_execution(ex)

  def delete_execution(self, query_uuid: str) -> bool:
    with self._lock, self._conn:
      self._conn.execute("DELETE FROM results WHERE query_uuid = ?",
                         (query_uuid,))
      cur = self._conn.execute("DELETE FROM executions WHERE query_uuid = ?",
                               (query_uuid,))
      return cur.rowcount > 0

  def close(self) -> None:
    with self._lock:
      self._conn.close()
