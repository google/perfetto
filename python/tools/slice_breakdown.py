#!/usr/bin/env python3
# Copyright (C) 2021 The Android Open Source Project
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
""" Given a trace file, gives the self-time of userspace slices broken
down by process, thread and thread state.
"""

import argparse
import sys
import os

PYTHON_DIR = os.path.join(
    os.path.dirname(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "python")
sys.path.append(os.path.join(PYTHON_DIR))

from perfetto.experimental.slice_breakdown import compute_breakdown
from perfetto.experimental.slice_breakdown import compute_breakdown_for_startup
from perfetto.trace_processor import TraceProcessor
from perfetto.trace_processor import TraceProcessorConfig


def compute_breakdown_wrapper(args):
  config = TraceProcessorConfig(bin_path=args.shell_path, verbose=args.verbose)
  with TraceProcessor(trace=args.file, config=config) as tp:
    if args.startup_bounds:
      breakdown = compute_breakdown_for_startup(tp, args.startup_package,
                                                args.process_name)
    else:
      breakdown = compute_breakdown(tp, args.start_ts, args.end_ts,
                                    args.process_name)
  return breakdown


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--file', required=True)
  parser.add_argument('--shell-path', default=None)
  parser.add_argument('--start-ts', default=None)
  parser.add_argument('--end-ts', default=None)
  parser.add_argument('--startup-bounds', action='store_true', default=False)
  parser.add_argument('--startup-package', default=None)
  parser.add_argument('--process-name', default=None)
  parser.add_argument('--verbose', action='store_true', default=False)
  parser.add_argument('--out-csv', required=True)
  args = parser.parse_args()

  if (args.start_ts or args.end_ts) and args.startup_bounds:
    print("Cannot specify --start-ts or --end-ts and --startup-bounds")
    return 1

  if args.startup_package and not args.startup_bounds:
    print("Must specify --startup-bounds if --startup-package is specified")
    return 1

  breakdown = compute_breakdown_wrapper(args)

  if args.out_csv:
    diff_csv = breakdown.to_csv(index=False)
    if args.out_csv == '-':
      sys.stdout.write(diff_csv)
    else:
      with open(args.out_csv, 'w') as out:
        out.write(diff_csv)

  return 0


if __name__ == '__main__':
  exit(main())
