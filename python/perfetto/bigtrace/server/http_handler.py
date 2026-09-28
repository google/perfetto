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
"""HTTP request handling and routing for the Bigtrace UI API."""

import http.server
import json
import logging
import os
import re
import shutil
from typing import Any, Dict, Tuple
import urllib.parse

from perfetto.bigtrace.server.catalog import (
    DEFAULT_PRESETS,
    SettingsOverrides,
    TraceCatalog,
)
from perfetto.bigtrace.server.executor import (
    BigtraceQueryExecutor,
    QueryFailedError,
)
from perfetto.bigtrace.server.util import as_bool, as_int, get_param, to_wire

logger = logging.getLogger(__name__)

# Origins that may read responses cross-origin, in addition to any localhost
# origin. Traces can hold private data, so arbitrary websites must not.
DEFAULT_CORS_ORIGINS = ("https://ui.perfetto.dev",)


class NotFoundError(Exception):
  pass


def experiment_to_wire(e: Dict[str, Any]) -> Dict[str, Any]:
  return {
      "experimentId":
          as_int(get_param(e, "experiment_id"), 0),
      "experimentName":
          str(get_param(e, "experiment_name", "")),
      "controlId":
          as_int(get_param(e, "control_id"), 0),
      "controlName":
          str(get_param(e, "control_name", "")),
      "isExperimentDenied":
          as_bool(get_param(e, "is_experiment_denied", False)),
      "isControlDenied":
          as_bool(get_param(e, "is_control_denied", False)),
  }


class BigtraceHandler(http.server.BaseHTTPRequestHandler):
  """Serves the Bigtrace REST API; see ROUTES for the endpoints."""

  executor: BigtraceQueryExecutor
  catalog: TraceCatalog
  cors_origins: Tuple[str, ...] = DEFAULT_CORS_ORIGINS

  def do_OPTIONS(self):
    self.send_response(200)
    self._send_cors_headers()
    self.send_header("Content-Length", "0")
    self.end_headers()

  def do_HEAD(self):
    self.do_OPTIONS()

  def do_GET(self):
    self._dispatch("GET")

  def do_POST(self):
    self._dispatch("POST")

  def do_DELETE(self):
    self._dispatch("DELETE")

  def log_message(self, fmt: str, *args: Any):
    logger.debug("%s %s", self.address_string(), fmt % args)

  def _origin_allowed(self, origin: str) -> bool:
    if "*" in self.cors_origins or origin in self.cors_origins:
      return True
    return urllib.parse.urlparse(origin).hostname in ("localhost", "127.0.0.1",
                                                      "::1")

  def _send_cors_headers(self):
    origin = self.headers.get("Origin")
    if origin is None:
      # Not a cross-origin browser request (e.g. curl), so nothing to protect.
      self.send_header("Access-Control-Allow-Origin", "*")
    elif self._origin_allowed(origin):
      # The UI sends credentials, which browsers only allow with this header.
      self.send_header("Access-Control-Allow-Origin", origin)
      self.send_header("Access-Control-Allow-Credentials", "true")
      self.send_header("Vary", "Origin")
      # Chrome asks before a public site (ui.perfetto.dev) calls localhost.
      if self.headers.get("Access-Control-Request-Private-Network") == "true":
        self.send_header("Access-Control-Allow-Private-Network", "true")
    self.send_header("Access-Control-Allow-Methods", "GET, POST, DELETE")
    self.send_header("Access-Control-Allow-Headers",
                     "Content-Type, Authorization")

  def _send_json(self, status: int, data: Any):
    body = json.dumps(data).encode("utf-8")
    self.send_response(status)
    self._send_cors_headers()
    self.send_header("Content-Type", "application/json; charset=utf-8")
    self.send_header("Content-Length", str(len(body)))
    self.end_headers()
    self.wfile.write(body)

  def _send_error(self, status: int, detail: str):
    self._send_json(status, {"detail": detail})

  def _read_json_body(self) -> Dict[str, Any]:
    length = as_int(self.headers.get("Content-Length"), 0)
    if length <= 0:
      return {}
    body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
    if not isinstance(body, dict):
      raise ValueError("Request body must be a JSON object")
    return body

  def _normalize_path(self) -> str:
    """Strips the query string, duplicate slashes and a /v1 prefix."""
    path = re.sub(r"/+", "/", urllib.parse.urlparse(self.path).path)
    if path == "/v1" or path.startswith("/v1/"):
      path = path[3:]
    return path.rstrip("/") or "/"

  def _dispatch(self, method: str):
    path = self._normalize_path()
    try:
      body = self._read_json_body() if method == "POST" else {}
      for route_method, pattern, handler in ROUTES:
        match = pattern.fullmatch(path) if route_method == method else None
        if match:
          return handler(self, body, *match.groups())
      raise NotFoundError(f"Route not found: {path}")
    except NotFoundError as e:
      self._send_error(404, str(e))
    except (BrokenPipeError, ConnectionResetError):
      logger.debug("Client closed connection during %s %s", method, path)
    except (ValueError, TypeError, AttributeError, QueryFailedError) as e:
      logger.info("Bad request %s %s: %s", method, path, e)
      self._send_error(400, str(e))
    except Exception as e:  # pylint: disable=broad-except
      logger.exception("Error handling %s %s", method, path)
      self._send_error(500, str(e))

  # Catalog endpoints -------------------------------------------------------

  def _root(self, _body):
    self._send_json(
        200, {
            "status": "ok",
            "service": "bigtrace",
            "traceCount": len(self.catalog.get_traces()),
            "traceDir": self.catalog.trace_dir,
            "recursive": self.catalog.recursive,
        })

  def _refresh(self, _body):
    self.catalog.reload()
    self._send_json(200, {
        "status": "ok",
        "traceCount": len(self.catalog.get_traces())
    })

  def _reset_session(self, _body):
    """Forgets tables, modules etc. created by earlier queries."""
    self.executor.pool.reset_session()
    self._send_json(200, {"status": "ok"})

  def _execution_config(self, _body):
    manifest = self.catalog.manifest_path or ""
    self._send_json(
        200, {
            "setting": [
                {
                    "id": "trace_directory",
                    "name": "Trace directory",
                    "description": "Directory on the server holding traces",
                    "category": "TRACE_ADDRESS",
                    "plainString": {
                        "defaultValue": self.catalog.trace_dir
                    },
                },
                {
                    "id": "recursive",
                    "name": "Recursive scan",
                    "description": "Also scan subdirectories",
                    "category": "TRACE_ADDRESS",
                    "booleanOptions": {
                        "defaultValue": self.catalog.recursive
                    },
                },
                {
                    "id": "trace_manifest",
                    "name": "Trace manifest",
                    "description": "Path to a JSON or CSV manifest",
                    "category": "TRACE_ADDRESS",
                    "plainString": {
                        "defaultValue": manifest
                    },
                },
                {
                    "id": "trace_uuids",
                    "name": "Trace UUIDs",
                    "description": "Only these trace UUIDs or file names",
                    "category": "TRACE_ADDRESS",
                    "stringArray": {
                        "defaultValues": []
                    },
                },
            ]
        })

  def _metadata_schema(self, body):
    self.catalog.apply_overrides(SettingsOverrides.parse(body.get("settings")))
    self._send_json(200, {"columns": self.catalog.get_schema()})

  def _metadata(self, body):
    overrides = SettingsOverrides.parse(body.get("settings"))
    self.catalog.apply_overrides(overrides)
    traces, total = self.catalog.filter_and_sort(
        filters=body.get("filters"),
        order_by=body.get("order_by"),
        experiment_filter=get_param(body, "experiment_filter"),
        trace_uuids=overrides.trace_uuids,
        limit=as_int(body.get("limit"), 50),
        offset=as_int(body.get("offset"), 0),
    )
    columns = body.get("columns") or [
        c["name"]
        for c in self.catalog.get_schema()
        if c.get("defaultVisible", True)
    ]
    self._send_json(
        200, {
            "columnNames": columns,
            "rows": [{
                "values": [to_wire(t.get_value(c)) for c in columns]
            } for t in traces],
            "totalFilteredRows": total,
        })

  def _presets(self, _body):
    presets = self.catalog.get_presets() or DEFAULT_PRESETS
    self._send_json(200, {"tracePresets": presets})

  def _experiments(self, body):
    query = str(body.get("search_query") or "").lower()
    limit = max(0, as_int(body.get("limit"), 50))
    offset = max(0, as_int(body.get("offset"), 0))
    matches = [
        e for e in map(experiment_to_wire, self.catalog.get_experiments())
        if query in e["experimentName"].lower() or
        query in e["controlName"].lower()
    ]
    self._send_json(200, {"experiments": matches[offset:offset + limit]})

  def _experiment_metadata(self, body):
    wanted = as_int(body.get("experiment_id"), None)
    for exp in map(experiment_to_wire, self.catalog.get_experiments()):
      if exp["experimentId"] == wanted:
        return self._send_json(200, {"experiment": exp})
    self._send_json(200, {})

  def _trace_file(self, _body, trace_uuid):
    trace = next(
        (t for t in self.catalog.get_traces() if t.trace_uuid == trace_uuid),
        None)
    if not trace or not os.path.isfile(trace.file_path):
      raise NotFoundError("Trace file not found")
    with open(trace.file_path, "rb") as f:
      self.send_response(200)
      self._send_cors_headers()
      self.send_header("Content-Type", "application/octet-stream")
      self.send_header("Content-Disposition",
                       f'attachment; filename="{trace.file_name}"')
      self.send_header("Content-Length", str(os.fstat(f.fileno()).st_size))
      self.end_headers()
      shutil.copyfileobj(f, self.wfile, 1 << 16)

  # Query endpoints ---------------------------------------------------------

  def _execute_sync(self, body):
    self._send_json(200, self.executor.execute(body, is_async=False))

  def _execute_async(self, body):
    self._send_json(200, self.executor.execute(body, is_async=True))

  def _list_executions(self, _body):
    self._send_json(200, {"queryExecutions": self.executor.list_executions()})

  def _get_execution(self, _body, query_uuid):
    ex = self.executor.get_execution(query_uuid)
    if ex is None:
      raise NotFoundError(f"Query {query_uuid} not found")
    self._send_json(200, ex)

  def _get_status(self, _body, query_uuid):
    status = self.executor.get_status(query_uuid)
    if status is None:
      raise NotFoundError(f"Query {query_uuid} not found")
    self._send_json(200, status)

  def _fetch_results(self, body, query_uuid):
    try:
      result = self.executor.fetch_results(
          query_uuid,
          offset=as_int(body.get("offset"), 0),
          limit=as_int(body.get("limit"), 50),
          order_by=body.get("order_by"),
          filters=body.get("filters"),
          columns=body.get("columns"),
      )
    except KeyError:
      raise NotFoundError(f"Query {query_uuid} not found") from None
    self._send_json(200, result)

  def _cancel(self, _body, query_uuid):
    self.executor.cancel_query(query_uuid)
    self._send_json(200, {})

  def _delete(self, _body, query_uuid):
    self.executor.delete_execution(query_uuid)
    self._send_json(200, {})

  def _check_table_exists(self, body):
    table_name = str(body.get("table_name", ""))
    self._send_json(200, self.executor.check_table_exists(table_name))


_UUID = r"([^/:]+)"
ROUTES = [
    (method, re.compile(pattern), handler) for method, pattern, handler in [
        ("GET", r"/", BigtraceHandler._root),
        ("POST", r"/refresh", BigtraceHandler._refresh),
        ("POST", r"/session/reset", BigtraceHandler._reset_session),
        ("GET", r"/trace_presets", BigtraceHandler._presets),
        ("GET", rf"/trace/{_UUID}/[^/]+", BigtraceHandler._trace_file),
        ("GET", r"/query_executions", BigtraceHandler._list_executions),
        ("GET", rf"/query_executions/{_UUID}", BigtraceHandler._get_execution),
        ("GET", rf"/query_executions/{_UUID}:status",
         BigtraceHandler._get_status),
        ("POST", r"/bigtrace_execution_config",
         BigtraceHandler._execution_config),
        ("POST", r"/trace_metadata_schema", BigtraceHandler._metadata_schema),
        ("POST", r"/trace_metadata", BigtraceHandler._metadata),
        ("POST", r"/experiments", BigtraceHandler._experiments),
        ("POST", r"/experiment_metadata", BigtraceHandler._experiment_metadata),
        ("POST", r"/execute_bigtrace_query", BigtraceHandler._execute_sync),
        ("POST", r"/execute_bigtrace_query_async",
         BigtraceHandler._execute_async),
        ("POST", rf"/query_executions/{_UUID}:fetch_results",
         BigtraceHandler._fetch_results),
        ("POST", rf"/query_executions/{_UUID}:cancel", BigtraceHandler._cancel),
        ("POST", r"/check_table_exists", BigtraceHandler._check_table_exists),
        ("DELETE", rf"/query_executions/{_UUID}", BigtraceHandler._delete),
    ]
]
