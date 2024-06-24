# Copyright (C) 2024 The Android Open Source Project
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

import unittest
import subprocess
import perfetto.bigtrace.api
import os

from perfetto.common.exceptions import PerfettoException


class BigtraceTest(unittest.TestCase):

  @classmethod
  def setUpClass(self):
    self.root_dir = os.environ["ROOT_DIR"]
    self.worker = subprocess.Popen(os.environ["WORKER_PATH"])
    self.orchestrator = subprocess.Popen(os.environ["ORCHESTRATOR_PATH"])
    self.client = perfetto.bigtrace.api.Bigtrace()

  @classmethod
  def tearDownClass(self):
    self.worker.kill()
    self.orchestrator.kill()
    del self.client

  def test_valid_traces(self):
    result = self.client.query([
        f"{self.root_dir}/test/data/api24_startup_cold.perfetto-trace",
        f"{self.root_dir}/test/data/api24_startup_hot.perfetto-trace"
    ], "SELECT count(1) as count FROM slice LIMIT 5")

    self.assertEqual(result['count'][0], 9726)
    self.assertEqual(result['count'][1], 5726)

  def test_empty_traces(self):
    with self.assertRaises(PerfettoException):
      result = self.client.query([], "SELECT count(1) FROM slice LIMIT 5")

  def test_empty_sql_string(self):
    with self.assertRaises(PerfettoException):
      result = self.client.query([
          f"{self.root_dir}/test/data/api24_startup_cold.perfetto-trace",
          f"{self.root_dir}/test/data/api24_startup_hot.perfetto-trace"
      ], "")

  def test_message_limit_exceeded(self):
    with self.assertRaises(PerfettoException):
      result = self.client.query(
          [f"{self.root_dir}/test/data/long_task_tracking_trace"],
          "SELECT * FROM slice")
