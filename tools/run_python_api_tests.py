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

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def main():
  # Append test and src paths so that all imports are loaded in correctly
  sys.path.append(os.path.join(ROOT_DIR, 'test', 'trace_processor', 'python'))
  sys.path.append(
      os.path.join(ROOT_DIR, 'src', 'trace_processor', 'python', 'perfetto'))
  import api_unittest
  import api_integrationtest

  # Set paths to trace_processor_shell and root directory as environment
  # variables
  parser = argparse.ArgumentParser()
  parser.add_argument("shell", type=str)
  os.environ["SHELL_PATH"] = parser.parse_args().shell
  os.environ["ROOT_DIR"] = ROOT_DIR

  # Initialise test suite
  loader = unittest.TestLoader()
  suite = unittest.TestSuite()

  # Add all relevant tests to test suite
  suite.addTests(loader.loadTestsFromModule(api_unittest))
  suite.addTests(loader.loadTestsFromModule(api_integrationtest))

  # Initialise runner to run all tests in suite
  runner = unittest.TextTestRunner(verbosity=3)
  result = runner.run(suite)

  return 0 if result.wasSuccessful() else 1


if __name__ == '__main__':
  sys.exit(main())
