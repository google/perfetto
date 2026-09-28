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
from typing import Any, Dict, List, Optional, Tuple

from perfetto.bigtrace.server.util import iso_now

HISTORY_DB_NAME = ".bigtrace_history.db"

# Execution keys that only mean something while the server is running.
_RUNTIME_KEYS = ("_rows", "_cancel", "_active", "_claimed_rows")

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
  columns, for crash recovery and ordering. Results are stored in JSON chunks.
  """

  SCHEMA_VERSION = 1

  def __init__(self, db_path: Optional[str] = None):
    self.db_path = ":memory:"
    if db_path and db_path != ":memory:":
      self.db_path = os.path.abspath(db_path)
      os.makedirs(os.path.dirname(self.db_path), exist_ok=True)
    self._lock = threading.Lock()
    self._conn = sqlite3.connect(self.db_path, check_same_thread=False)
    self._init_schema()

  def _init_schema(self) -> None:
    with self._lock:
      if self.db_path != ":memory:":
        self._conn.execute("PRAGMA journal_mode=WAL")
        self._conn.execute("PRAGMA synchronous=NORMAL")
      with self._conn:
        version = self._conn.execute("PRAGMA user_version").fetchone()[0]
        if version != self.SCHEMA_VERSION:
          # History is only a record of past queries: drop it, don't migrate.
          self._conn.execute("DROP TABLE IF EXISTS executions")
          self._conn.execute("DROP TABLE IF EXISTS results")
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
              rows TEXT NOT NULL,
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

  def save_results(self,
                   query_uuid: str,
                   rows: List[List[Any]],
                   chunk_size: int = 5000) -> None:
    chunks = [(query_uuid, i // chunk_size,
               json.dumps(rows[i:i + chunk_size], default=str))
              for i in range(0, len(rows), chunk_size)]
    with self._lock, self._conn:
      self._conn.execute("DELETE FROM results WHERE query_uuid = ?",
                         (query_uuid,))
      self._conn.executemany("INSERT INTO results VALUES (?, ?, ?)", chunks)

  def get_results(self, query_uuid: str) -> List[List[Any]]:
    with self._lock:
      chunks = self._conn.execute(
          "SELECT rows FROM results WHERE query_uuid = ? ORDER BY chunk",
          (query_uuid,)).fetchall()
    return [row for (chunk,) in chunks for row in json.loads(chunk)]

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
