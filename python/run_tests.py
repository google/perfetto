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
import os
import sys
import unittest

from test import api_integrationtest
from test import bigtrace_api_integrationtest
from test import query_result_iterator_unittest
from test import resolver_unittest
from test import stdlib_unittest

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.append(os.path.join(ROOT_DIR))


def main():
  try:
    import numpy
    import pandas
  except ModuleNotFoundError:
    print('Cannot proceed. Please `pip3 install pandas numpy`', file=sys.stderr)
    return 1

  # Set paths to trace_processor_shell and root directory as environment
  # variables
  parser = argparse.ArgumentParser()
  parser.add_argument("host_out_path", type=str)
  host_out_path = parser.parse_args().host_out_path
  os.environ["SHELL_PATH"] = f"{host_out_path}/trace_processor_shell"
  os.environ["ORCHESTRATOR_PATH"] = f"{host_out_path}/orchestrator_main"
  os.environ["WORKER_PATH"] = f"{host_out_path}/worker_main"
  os.environ["ROOT_DIR"] = ROOT_DIR

  loader = unittest.TestLoader()
  suite = unittest.TestSuite()

  # Add all relevant tests to test suite
  suite.addTests(loader.loadTestsFromModule(query_result_iterator_unittest))
  suite.addTests(loader.loadTestsFromModule(resolver_unittest))
  suite.addTests(loader.loadTestsFromModule(api_integrationtest))
  suite.addTests(loader.loadTestsFromModule(stdlib_unittest))
  if os.path.exists(os.environ["WORKER_PATH"]):
    suite.addTests(loader.loadTestsFromModule(bigtrace_api_integrationtest))

  # Initialise runner to run all tests in suite
  runner = unittest.TextTestRunner(verbosity=3)
  result = runner.run(suite)

  return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
  sys.exit(main())
