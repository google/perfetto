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
import cmd
import logging
import numpy as np
import pandas as pd
import plotille

from perfetto.batch_trace_processor.api import BatchTraceProcessor, BatchTraceProcessorConfig
from perfetto.trace_processor import TraceProcessorException, TraceProcessorConfig
from typing import List


class TpBatchShell(cmd.Cmd):

  def __init__(self, files: List[str], batch_tp: BatchTraceProcessor):
    super().__init__()
    self.files = files
    self.batch_tp = batch_tp

  def do_table(self, arg: str):
    try:
      data = self.batch_tp.query_and_flatten(arg)
      print(data)
    except TraceProcessorException as ex:
      logging.error("Query failed: {}".format(ex))

  def do_histogram(self, arg: str):
    try:
      data = self.batch_tp.query_single_result(arg)
      print(plotille.histogram(data))
      self.print_percentiles(data)
    except TraceProcessorException as ex:
      logging.error("Query failed: {}".format(ex))

  def do_vhistogram(self, arg: str):
    try:
      data = self.batch_tp.query_single_result(arg)
      print(plotille.hist(data))
      self.print_percentiles(data)
    except TraceProcessorException as ex:
      logging.error("Query failed: {}".format(ex))

  def do_count(self, arg: str):
    try:
      data = self.batch_tp.query_single_result(arg)
      counts = dict()
      for i in data:
        counts[i] = counts.get(i, 0) + 1
      print(counts)
    except TraceProcessorException as ex:
      logging.error("Query failed: {}".format(ex))

  def do_close(self, _):
    return True

  def do_quit(self, _):
    return True

  def do_EOF(self, _):
    print("")
    return True

  def print_percentiles(self, data):
    percentiles = [25, 50, 75, 95, 99, 99.9]
    nearest = np.percentile(data, percentiles, interpolation='nearest')
    logging.info("Representative traces for percentiles")
    for i, near in enumerate(nearest):
      print("{}%: {}".format(percentiles[i], self.files[data.index(near)]))


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--shell-path', default=None)
  parser.add_argument('--verbose', action='store_true', default=False)
  parser.add_argument('--file-list', default=None)
  parser.add_argument('--query-file', default=None)
  parser.add_argument('--interactive', default=None)
  parser.add_argument('files', nargs='*')
  args = parser.parse_args()

  logging.basicConfig(level=logging.DEBUG)

  files = args.files
  if args.file_list:
    with open(args.file_list, 'r') as f:
      files += f.read().splitlines()

  if not files:
    logging.info("At least one file must be specified in files or file list")

  logging.info('Loading traces...')
  config = BatchTraceProcessorConfig(
      tp_config=TraceProcessorConfig(
          bin_path=args.shell_path,
          verbose=args.verbose,
      ))

  with BatchTraceProcessor(files, config) as batch_tp:
    if args.query_file:
      logging.info('Running query file...')

      with open(args.query_file, 'r') as f:
        queries_str = f.read()

      queries = [q.strip() for q in queries_str.split(";\n")]
      for q in queries[:-1]:
        batch_tp.query(q)

      res = batch_tp.query_and_flatten(queries[-1])
      print(res.to_csv(index=False))

    if args.interactive or not args.query_file:
      try:
        TpBatchShell(files, batch_tp).cmdloop()
      except KeyboardInterrupt:
        pass

    logging.info("Closing; please wait...")


if __name__ == '__main__':
  exit(main())
