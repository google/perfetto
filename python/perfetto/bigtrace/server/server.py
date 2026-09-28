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
"""Wires the catalog, executor and HTTP server together."""

import dataclasses as dc
import http.server
import logging
import os
import signal
import sqlite3
import subprocess
import threading
import urllib.parse
from typing import Optional, Tuple

from perfetto.trace_processor.api import TraceProcessorConfig
from perfetto.bigtrace.server.catalog import DEFAULT_UI_ORIGIN, TraceCatalog
from perfetto.bigtrace.server.executor import BigtraceQueryExecutor
from perfetto.bigtrace.server.history import BigtraceHistoryDb, HISTORY_DB_NAME
from perfetto.bigtrace.server.http_handler import (
    BigtraceHandler,
    DEFAULT_CORS_ORIGINS,
)
from perfetto.bigtrace.server.snapshot import SnapshotStore
from perfetto.bigtrace.server.util import (
    default_worker_count,
    resolve_trace_processor_bin,
)

logger = logging.getLogger(__name__)


def _origin(url: str) -> str:
  parts = urllib.parse.urlsplit(url)
  return f"{parts.scheme}://{parts.netloc}"


@dc.dataclass
class BigtraceServerConfig:
  trace_dir: Optional[str] = None
  recursive: bool = False
  # JSON manifest, or CSV if the path ends in .csv.
  manifest_path: Optional[str] = None
  host: str = "127.0.0.1"
  port: int = 5001
  max_workers: Optional[int] = None
  bin_path: Optional[str] = None
  # Defaults to HISTORY_DB_NAME inside the trace directory.
  history_db: Optional[str] = None
  # Traces kept loaded between queries; 0 loads every trace for every query.
  pool_size: int = 0
  # Parse-once snapshots of loaded traces (snapshot.py); None disables them.
  snapshot_dir: Optional[str] = None
  snapshot_budget_gb: float = 50
  # Origins allowed besides DEFAULT_CORS_ORIGINS and localhost; "*" for any.
  cors_origins: Tuple[str, ...] = ()
  # Perfetto UI that trace links open in; its origin is allowed to fetch traces.
  ui_origin: str = DEFAULT_UI_ORIGIN


class BigtraceServer:

  def __init__(self, config: Optional[BigtraceServerConfig] = None):
    self.config = config or BigtraceServerConfig()
    cfg = self.config
    cfg.max_workers = cfg.max_workers or default_worker_count()

    handler = type(
        "Handler", (BigtraceHandler,), {
            "cors_origins":
                DEFAULT_CORS_ORIGINS + tuple(cfg.cors_origins) +
                (_origin(cfg.ui_origin),)
        })
    self.httpd = http.server.ThreadingHTTPServer((cfg.host, cfg.port), handler)
    self.server_port = self.httpd.server_address[1]
    link_host = "127.0.0.1" if cfg.host in ("", "0.0.0.0", "::") else cfg.host

    self.catalog = TraceCatalog(
        trace_dir=cfg.trace_dir,
        recursive=cfg.recursive,
        manifest_path=cfg.manifest_path,
        base_url=f"http://{link_host}:{self.server_port}",
        ui_origin=cfg.ui_origin,
    )
    self.history_db = self._open_history_db()
    bin_path = resolve_trace_processor_bin(cfg.bin_path)
    self.snapshots: Optional[SnapshotStore] = None
    if cfg.snapshot_dir and cfg.snapshot_budget_gb > 0 and bin_path:
      try:
        self.snapshots = SnapshotStore(cfg.snapshot_dir,
                                       int(cfg.snapshot_budget_gb * 2**30),
                                       bin_path)
      except (OSError, subprocess.SubprocessError) as e:
        logger.warning("Snapshots disabled: %s", e)
    self.executor = BigtraceQueryExecutor(
        catalog=self.catalog,
        tp_config=TraceProcessorConfig(bin_path=bin_path),
        max_workers=cfg.max_workers,
        history_db=self.history_db,
        pool_size=cfg.pool_size,
        snapshots=self.snapshots,
    )
    handler.executor = self.executor
    handler.catalog = self.catalog
    self._thread: Optional[threading.Thread] = None
    self._serving = False
    self._stopped = False

  def _open_history_db(self) -> BigtraceHistoryDb:
    path = self.config.history_db or os.path.join(self.catalog.trace_dir,
                                                  HISTORY_DB_NAME)
    try:
      return BigtraceHistoryDb(path)
    except (OSError, sqlite3.Error) as e:
      logger.warning("Cannot open history at %s (%s); keeping it in memory",
                     path, e)
      return BigtraceHistoryDb(":memory:")

  def start(self, background: bool = False) -> None:
    self._serving = True
    if background:
      self._thread = threading.Thread(
          target=self.httpd.serve_forever, daemon=True)
      self._thread.start()
      return
    # Treat SIGTERM like Ctrl-C so warm TraceProcessors are shut down.
    signal.signal(signal.SIGTERM, signal.default_int_handler)
    try:
      self.httpd.serve_forever()
    except KeyboardInterrupt:
      pass
    finally:
      self.stop()

  def stop(self) -> None:
    if self._stopped:
      return
    self._stopped = True
    if self._serving:
      self.httpd.shutdown()
      if self._thread:
        self._thread.join()
    self.httpd.server_close()
    self.executor.close()
    if self.snapshots:
      self.snapshots.close()
