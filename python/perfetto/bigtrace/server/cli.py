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
"""Command line entry point: tools/bigtrace_server.py."""

import argparse
import logging
import os
from typing import List, Optional

from perfetto.bigtrace.server.archive import (DEFAULT_CACHE_DIR,
                                              extract_archive, is_archive)
from perfetto.bigtrace.server.catalog import DEFAULT_UI_ORIGIN
from perfetto.bigtrace.server.history import HISTORY_DB_NAME
from perfetto.bigtrace.server.manifest import generate_manifest
from perfetto.bigtrace.server.server import BigtraceServer, BigtraceServerConfig
from perfetto.bigtrace.server.util import MANIFEST_NAME, default_worker_count

DEFAULT_SNAPSHOT_DIR = os.path.join("~", ".cache", "perfetto", "bigtrace",
                                    "snapshots")

# Defaults are for interactive use: keep traces loaded and snapshot them so
# repeat queries are fast. --batch keeps nothing; explicit flags win over both.
_DEFAULTS = dict(pool_size=64, snapshot_budget_gb=50)
_BATCH = dict(pool_size=0, snapshot_budget_gb=0)


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
  parser = argparse.ArgumentParser(
      description="Serves the Bigtrace UI API over a directory of traces.")
  parser.add_argument(
      "traces",
      nargs="?",
      default=".",
      metavar="TRACES",
      help="Directory of traces, or a zip/tar archive of them, which is "
      f"extracted to {DEFAULT_CACHE_DIR} (default: current directory)")
  parser.add_argument(
      "-r", "--recursive", action="store_true", help="Scan subdirectories")
  parser.add_argument(
      "-m",
      "--manifest",
      help="Manifest to load: JSON, CSV (.csv) or a trace_fetch SQLite "
      f"metadata.db (.db) (default: {MANIFEST_NAME} or metadata.db in the "
      "trace directory, if present)")
  parser.add_argument(
      "--batch",
      action="store_true",
      help="For one pass over many traces, e.g. scripted sweeps: keep no "
      "traces loaded and make no snapshots (--pool-size 0 "
      "--snapshot-budget-gb 0)")
  parser.add_argument("-p", "--port", type=int, default=5001)
  parser.add_argument("--host", default="127.0.0.1")
  parser.add_argument(
      "-w",
      "--workers",
      type=int,
      help=f"Traces queried in parallel (default: {default_worker_count()})")
  parser.add_argument(
      "--shell-path", help="Path to trace_processor_shell to use")
  parser.add_argument(
      "--history-db",
      metavar="PATH",
      help=f"Query history database (default: {HISTORY_DB_NAME} in the trace "
      "directory)")
  parser.add_argument(
      "--pool-size",
      type=int,
      help="Traces kept loaded between queries, least recently used are "
      "closed; 0 loads every trace for every query (default: 64)")
  parser.add_argument(
      "--snapshot-dir",
      default=DEFAULT_SNAPSHOT_DIR,
      help="Where parse-once snapshots of traces are kept (default: "
      "%(default)s)")
  parser.add_argument(
      "--snapshot-budget-gb",
      type=float,
      help="Disk space for parse-once snapshots, least recently used are "
      "deleted; 0 disables them (default: 50)")
  parser.add_argument(
      "--cors-origin",
      action="append",
      default=[],
      metavar="ORIGIN",
      help="Extra origin allowed to call the server; repeatable. Localhost "
      "and https://ui.perfetto.dev are always allowed")
  parser.add_argument(
      "--origin",
      default=DEFAULT_UI_ORIGIN,
      help="Perfetto UI that trace links open in (default: %(default)s)")

  manifest = parser.add_argument_group("manifest generation")
  manifest.add_argument(
      "--generate-manifest",
      nargs="?",
      const=MANIFEST_NAME,
      metavar="OUT",
      help="Update the manifest before serving (default OUT: "
      f"{MANIFEST_NAME} in the trace directory)")
  manifest.add_argument(
      "-f",
      "--force",
      action="store_true",
      help="Re-extract every trace instead of reusing cached entries")
  manifest.add_argument(
      "--metadata-query", help="Extra SQL whose first row is added per trace")
  args = parser.parse_args(argv)
  if args.shell_path and not (os.path.isfile(args.shell_path) and
                              os.access(args.shell_path, os.X_OK)):
    parser.error(f"--shell-path is not an executable: {args.shell_path}")
  for name, value in (_BATCH if args.batch else _DEFAULTS).items():
    if getattr(args, name) is None:
      setattr(args, name, value)
  return args


def main(argv: Optional[List[str]] = None) -> None:
  logging.basicConfig(
      level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
  args = parse_args(argv)

  trace_dir, recursive = args.traces, args.recursive
  if is_archive(trace_dir):
    print(f"Unpacking {trace_dir}", flush=True)
    trace_dir, recursive = extract_archive(trace_dir), True

  server = BigtraceServer(
      BigtraceServerConfig(
          trace_dir=trace_dir,
          recursive=recursive,
          manifest_path=args.manifest,
          host=args.host,
          port=args.port,
          max_workers=args.workers,
          bin_path=args.shell_path,
          history_db=args.history_db,
          pool_size=args.pool_size,
          snapshot_dir=os.path.expanduser(args.snapshot_dir),
          snapshot_budget_gb=args.snapshot_budget_gb,
          cors_origins=tuple(args.cors_origin),
          ui_origin=args.origin,
      ))
  catalog = server.catalog

  if args.generate_manifest:
    out = os.path.join(catalog.trace_dir, args.generate_manifest)
    generate_manifest(
        trace_dir=catalog.trace_dir,
        output_path=out,
        recursive=recursive,
        max_workers=args.workers,
        bin_path=args.shell_path,
        metadata_query=args.metadata_query,
        force=args.force,
    )
    catalog.reload(manifest_path=out)

  url = f"http://{args.host}:{server.server_port}"
  source = catalog.manifest_path or catalog.trace_dir
  kept = f"up to {args.pool_size}" if args.pool_size > 0 else "none (--batch)"
  snapshots = server.snapshots.directory if server.snapshots else "off"
  print(
      f"Bigtrace server on {url}\n"
      f"  Traces:  {len(catalog.get_traces())} from {source}\n"
      f"  Workers: {server.config.max_workers}\n"
      f"  Kept loaded: {kept}\n"
      f"  Snapshots: {snapshots}\n"
      f"  History: {server.history_db.db_path}\n"
      f"Open http://localhost:10000/bigtrace.html?server={url}",
      flush=True)
  server.start()


if __name__ == "__main__":
  main()
