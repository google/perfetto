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
"""Wires the catalog, DuckDB engine, executor and HTTP server together."""

import dataclasses as dc
import http.server
import signal
import threading
import urllib.parse
from typing import Optional, Tuple

from perfetto.trace_processor.api import TraceProcessorConfig
from perfetto.bigtrace.server.catalog import DEFAULT_UI_ORIGIN, TraceCatalog
from perfetto.bigtrace.server.duckdb_engine import DuckDBEngine
from perfetto.bigtrace.server.executor import BigtraceQueryExecutor
from perfetto.bigtrace.server.http_handler import (
    BigtraceHandler,
    DEFAULT_CORS_ORIGINS,
)
from perfetto.bigtrace.server.util import (
    default_worker_count,
    resolve_trace_processor_bin,
)


def _origin(url: str) -> str:
  parts = urllib.parse.urlsplit(url)
  return f"{parts.scheme}://{parts.netloc}"


@dc.dataclass
class BigtraceServerConfig:
  trace_dir: Optional[str] = None
  recursive: bool = False
  # JSON manifest, CSV (.csv), or SQLite metadata.db (.db).
  manifest_path: Optional[str] = None
  host: str = "127.0.0.1"
  port: int = 5001
  max_workers: Optional[int] = None
  bin_path: Optional[str] = None
  # Optional Parquet directory for exporting/loading DuckDB tables; None disables.
  parquet_dir: Optional[str] = None
  # Optional DuckDB database file path; None uses an ephemeral file.
  duckdb_path: Optional[str] = None
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
                (_origin(cfg.ui_origin),),
            "configured_host":
                cfg.host,
        })
    self.httpd = http.server.ThreadingHTTPServer((cfg.host, cfg.port), handler)
    self.httpd.configured_host = cfg.host
    self.server_port = self.httpd.server_address[1]
    link_host = "127.0.0.1" if cfg.host in ("", "0.0.0.0", "::") else cfg.host

    self.catalog = TraceCatalog(
        trace_dir=cfg.trace_dir,
        recursive=cfg.recursive,
        manifest_path=cfg.manifest_path,
        base_url=f"http://{link_host}:{self.server_port}",
        ui_origin=cfg.ui_origin,
    )
    bin_path = resolve_trace_processor_bin(cfg.bin_path)
    self.duckdb = DuckDBEngine(
        db_path=cfg.duckdb_path, parquet_dir=cfg.parquet_dir)
    self.executor = BigtraceQueryExecutor(
        catalog=self.catalog,
        tp_config=TraceProcessorConfig(bin_path=bin_path),
        max_workers=cfg.max_workers,
        duckdb_engine=self.duckdb,
    )
    handler.executor = self.executor
    handler.catalog = self.catalog
    self._thread: Optional[threading.Thread] = None
    self._serving = False
    self._stopped = False

  def start(self, background: bool = False) -> None:
    self._serving = True
    if background:
      self._thread = threading.Thread(
          target=self.httpd.serve_forever, daemon=True)
      self._thread.start()
      return
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
