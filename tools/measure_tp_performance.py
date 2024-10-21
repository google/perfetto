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

import argparse
import os
import re
import signal
import sys
import subprocess

import psutil

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

REGEX = re.compile(
    '.*Trace loaded: ([0-9.]+) MB in ([0-9.]+)s \(([0-9.]+) MB/s\)')


def run_tp_until_ingestion(args, env):
  tp_args = [os.path.join(args.out, 'trace_processor_shell'), args.trace_file]
  if not args.ftrace_raw:
    tp_args.append('--no-ftrace-raw')
  tp_args.append('--dev')
  tp_args.append('--dev-flag drop-after-sort=true')
  tp = subprocess.Popen(
      tp_args,
      stdin=subprocess.PIPE,
      stdout=None if args.verbose else subprocess.DEVNULL,
      stderr=subprocess.PIPE,
      universal_newlines=True,
      env=env)

  lines = []
  while True:
    line = tp.stderr.readline()
    if args.verbose:
      sys.stderr.write(line)
    lines.append(line)

    match = REGEX.match(line)
    if match:
      break

    if tp.poll():
      break

  ret = tp.poll()
  fail = ret is not None and ret > 0
  if fail:
    print("Failed")
    for line in lines:
      sys.stderr.write(line)
  return tp, fail, match[2]


def heap_profile_run(args, dump_at_max: bool):
  profile_args = [
      os.path.join(ROOT_DIR, 'tools', 'heap_profile'), '-i', '1', '-n',
      'trace_processor_shell', '--print-config'
  ]
  if dump_at_max:
    profile_args.append('--dump-at-max')
  config = subprocess.check_output(
      profile_args,
      stderr=subprocess.DEVNULL,
  )

  out_file = os.path.join(
      args.result, args.result_prefix + ('max' if dump_at_max else 'rest'))
  perfetto_args = [
      os.path.join(args.out, 'perfetto'), '-c', '-', '--txt', '-o', out_file
  ]
  profile = subprocess.Popen(
      perfetto_args,
      stdin=subprocess.PIPE,
      stdout=None if args.verbose else subprocess.DEVNULL,
      stderr=None if args.verbose else subprocess.DEVNULL)
  profile.stdin.write(config)
  profile.stdin.close()

  env = {
      'LD_PRELOAD': os.path.join(args.out, 'libheapprofd_glibc_preload.so'),
      'TRACE_PROCESSOR_NO_MMAP': '1',
      'PERFETTO_HEAPPROFD_BLOCKING_INIT': '1'
  }
  (tp, fail, _) = run_tp_until_ingestion(args, env)

  profile.send_signal(signal.SIGINT)
  profile.wait()

  tp.stdin.close()
  tp.wait()

  if fail:
    os.remove(out_file)


def regular_run(args):
  env = {'TRACE_PROCESSOR_NO_MMAP': '1'}
  (tp, fail, time) = run_tp_until_ingestion(args, env)

  p = psutil.Process(tp.pid)
  mem = 0
  for m in p.memory_maps():
    mem += m.anonymous

  tp.stdin.close()
  tp.wait()

  print(f'Time taken: {time}s, Memory: {mem / 1024.0 / 1024.0}MB')


def only_sort_run(args):
  env = {
      'TRACE_PROCESSOR_NO_MMAP': '1',
  }
  (tp, fail, time) = run_tp_until_ingestion(args, env)

  tp.stdin.close()
  tp.wait()

  print(f'Time taken: {time}s')


def main():
  parser = argparse.ArgumentParser(
      description="This script measures the running time of "
      "ingesting a trace with trace processor as well as profiling "
      "trace processor's memory usage with heapprofd")
  parser.add_argument('--out', type=str, help='Out directory', required=True)
  parser.add_argument(
      '--result', type=str, help='Result directory', required=True)
  parser.add_argument(
      '--result-prefix', type=str, help='Result file prefix', required=True)
  parser.add_argument(
      '--ftrace-raw',
      action='store_true',
      help='Whether to ingest ftrace into raw table',
      default=False)
  parser.add_argument(
      '--kill-existing',
      action='store_true',
      help='Kill traced, perfetto_cmd and trace processor shell if running')
  parser.add_argument(
      '--verbose',
      action='store_true',
      help='Logs all stderr and stdout from subprocesses')
  parser.add_argument('trace_file', type=str, help='Path to trace')
  args = parser.parse_args()

  if args.kill_existing:
    subprocess.run(['killall', 'traced'],
                   stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    subprocess.run(['killall', 'perfetto'],
                   stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
    subprocess.run(['killall', 'trace_processor_shell'],
                   stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)

  traced = subprocess.Popen([os.path.join(args.out, 'traced')],
                            stdout=None if args.verbose else subprocess.DEVNULL,
                            stderr=None if args.verbose else subprocess.DEVNULL)
  print('Heap profile dump at max')
  heap_profile_run(args, dump_at_max=True)
  print('Heap profile dump at resting')
  heap_profile_run(args, dump_at_max=False)
  print('Regular run')
  regular_run(args)
  print('Only sort run')
  only_sort_run(args)

  traced.send_signal(signal.SIGINT)
  traced.wait()


if __name__ == "__main__":
  main()
