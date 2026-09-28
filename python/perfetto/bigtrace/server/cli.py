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
      help="Manifest to load: JSON, or CSV if it ends in .csv "
      f"(default: {MANIFEST_NAME} in the trace directory, if present)")
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
      "--keep-alive",
      action="store_true",
      help="Keep traces loaded between queries so repeat queries are fast")
  parser.add_argument(
      "--pool-size",
      type=int,
      default=50,
      help="Maximum loaded traces kept with --keep-alive (default: 50)")
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
  return parser.parse_args(argv)


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
          keep_alive=args.keep_alive,
          pool_size=args.pool_size,
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
  mode = " (keep-alive)" if args.keep_alive else ""
  print(
      f"Bigtrace server on {url}\n"
      f"  Traces:  {len(catalog.get_traces())} from {source}\n"
      f"  Workers: {server.config.max_workers}{mode}\n"
      f"  History: {server.history_db.db_path}\n"
      f"Open http://localhost:10000/bigtrace.html?server={url}",
      flush=True)
  server.start()


if __name__ == "__main__":
  main()
