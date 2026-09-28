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
"""HTTP backend for the Bigtrace UI that runs queries over local traces.

Traces come from a directory scan, a JSON manifest or a CSV manifest
(catalog.py); zip and tar archives are unpacked to a directory first
(archive.py). A query fans out over a thread pool with one TraceProcessor per
trace (executor.py, trace_processor_pool.py), loaded from a parse-once
snapshot when the trace has one (snapshot.py). Results are kept in memory for
paging and persisted to SQLite so history survives restarts (history.py).
http_handler.py maps the UI's REST API onto these; server.py wires them up and
cli.py is the tools/bigtrace_server.py entry point.
"""

from perfetto.bigtrace.server.catalog import TraceCatalog
from perfetto.bigtrace.server.cli import main
from perfetto.bigtrace.server.executor import BigtraceQueryExecutor
from perfetto.bigtrace.server.history import BigtraceHistoryDb
from perfetto.bigtrace.server.manifest import (BigtraceManifestGenerator,
                                               generate_manifest)
from perfetto.bigtrace.server.server import BigtraceServer, BigtraceServerConfig
from perfetto.bigtrace.server.snapshot import SnapshotStore
from perfetto.bigtrace.server.trace_processor_pool import TraceProcessorPool
from perfetto.bigtrace.server.util import resolve_trace_processor_bin
