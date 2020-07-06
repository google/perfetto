#!/usr/bin/env python3
# Copyright (C) 2020 The Android Open Source Project
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

from trace_processor.http import TraceProcessorHttp


def main():
  # Parse arguments passed from command line
  parser = argparse.ArgumentParser()
  parser.add_argument(
      "-a",
      "--address",
      help="Address at which trace_processor is being run, e.g. 127.0.0.1:9001",
      required=True,
      type=str)
  parser.add_argument(
      "-f", "--file", help="Absolute path to trace", required=True, type=str)
  args = parser.parse_args()

  # TODO(@aninditaghosh): Load trace into trace_processor_shell

  # Call functions on the loaded trace
  tp = TraceProcessorHttp(args.address)
  tp.notify_eof()
  print(tp.status())


if __name__ == "__main__":
  main()
