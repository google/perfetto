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
"""Shared helpers and fixture for the Bigtrace server tests."""

import contextlib
import json
import os
import shutil
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from typing import Any, Callable, Iterator, Optional, Tuple

from perfetto.bigtrace.server import (
    BigtraceServer,
    BigtraceServerConfig,
    resolve_trace_processor_bin,
)

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEST_DATA_DIR = os.path.join(REPO_ROOT, "test", "data")
EXAMPLES_DIR = os.path.join(REPO_ROOT, "examples", "bigtrace")
MANIFEST_JSON = os.path.join(EXAMPLES_DIR, "bigtrace_manifest.json")
MANIFEST_CSV = os.path.join(EXAMPLES_DIR, "bigtrace_manifest.csv")

COLD_TRACE = "api24_startup_cold.perfetto-trace"
HOT_TRACE = "api24_startup_hot.perfetto-trace"
WARM_TRACE = "api24_startup_warm.perfetto-trace"

# Runs until killed.
ENDLESS_SQL = ("WITH RECURSIVE n(i) AS "
               "(SELECT 0 UNION ALL SELECT i + 1 FROM n) "
               "SELECT count(1) FROM n")


def trace_path(name: str) -> str:
  return os.path.join(TEST_DATA_DIR, name)


def copy_trace(name: str, dst_dir: str, dst_name: str) -> str:
  """Copies test/data/<name> to <dst_dir>/<dst_name> and returns the path."""
  dst = os.path.join(dst_dir, dst_name)
  shutil.copyfile(trace_path(name), dst)
  return dst


def wait_for(condition: Callable[[], Any], timeout: float = 10.0) -> None:
  deadline = time.monotonic() + timeout
  while not condition():
    if time.monotonic() > deadline:
      raise AssertionError("Timed out waiting for condition")
    time.sleep(0.02)


def request_raw(base_url: str,
                method: str,
                path: str,
                body: Optional[dict] = None,
                headers: Optional[dict] = None) -> Tuple[int, bytes, dict]:
  """Sends a request and returns (status, raw body, headers), even on errors."""
  data = json.dumps(body).encode("utf-8") if body is not None else None
  all_headers = {"Content-Type": "application/json", **(headers or {})}
  req = urllib.request.Request(
      f"{base_url}{path}", data=data, headers=all_headers, method=method)
  try:
    with urllib.request.urlopen(req) as resp:
      return resp.status, resp.read(), resp.headers
  except urllib.error.HTTPError as e:
    with e:
      return e.code, e.read(), e.headers


def request(base_url: str,
            method: str,
            path: str,
            body: Optional[dict] = None) -> Tuple[int, dict, dict]:
  """Like request_raw() but decodes the body as JSON ({} if empty)."""
  status, raw, headers = request_raw(base_url, method, path, body)
  return status, json.loads(raw) if raw else {}, headers


@contextlib.contextmanager
def running_server(**config_overrides) -> Iterator[BigtraceServer]:
  """Runs a BigtraceServer on an ephemeral port in the background."""
  config = BigtraceServerConfig(
      **{
          "host": "127.0.0.1",
          "port": 0,
          "bin_path": resolve_trace_processor_bin(),
          **config_overrides
      })
  server = BigtraceServer(config)
  try:
    server.start(background=True)
    yield server
  finally:
    server.stop()


def base_url(server: BigtraceServer) -> str:
  return f"http://127.0.0.1:{server.server_port}"


class BigtraceServerTestCase(unittest.TestCase):
  """Runs one server per test class over test/data and the example manifest."""

  @classmethod
  def setUpClass(cls):
    cls.bin_path = resolve_trace_processor_bin()
    # Keep query history out of test/data.
    cls._history_dir = tempfile.TemporaryDirectory()
    cls._server_cm = running_server(
        trace_dir=TEST_DATA_DIR,
        manifest_path=MANIFEST_JSON,
        max_workers=4,
        history_db=os.path.join(cls._history_dir.name, "history.db"),
    )
    cls.server = cls._server_cm.__enter__()
    cls.base_url = base_url(cls.server)

  @classmethod
  def tearDownClass(cls):
    cls._server_cm.__exit__(None, None, None)
    cls._history_dir.cleanup()

  def setUp(self):
    self.server.catalog.reload(
        trace_dir=TEST_DATA_DIR, manifest_path=MANIFEST_JSON)

  def _request(self, method: str, path: str, body: Optional[dict] = None):
    return request(self.base_url, method, path, body)
