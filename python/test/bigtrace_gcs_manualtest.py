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

import os
import perfetto.bigtrace.api
import subprocess
import unittest

from perfetto.common.exceptions import PerfettoException

# To run this test you must setup the a GCS bucket and a GKE cluster setup with
# Bigtrace running.
# This should be executed within the same VPC to allow for connection to the
# service.

# This should be replaced with the name of the trace bucket you have deployed
# on.
TRACE_BUCKET_NAME = "trace_example_bucket"
# This should be loaded in the top level of the bucket.
TRACE_PATH = "android_startup_real.perfetto_trace"
# This should be replaced with the address of the Orchestrator service for the
# Bigtrace service.
ORCHESTRATOR_ADDRESS = "127.0.0.1:5052"
# This can be changed if testing on a different trace.
QUERY_RESULT_COUNT = 339338


class BigtraceGcsTest(unittest.TestCase):

  def setUpClass(self):
    self.client = perfetto.bigtrace.api.Bigtrace(
        wait_for_ready_for_testing=True)

  def test_valid_trace(self):
    traces = [f"/gcs/{TRACE_BUCKET_NAME}/o/{TRACE_PATH}"]
    result = self.client.query(traces, "SELECT count(1) as count FROM slice")
    self.assertEqual(result['count'].iloc[0], QUERY_RESULT_COUNT)

  def test_invalid_trace(self):
    with self.assertRaises(PerfettoException):
      traces = [f"/gcs/{TRACE_BUCKET_NAME}/o/badpath"]
      result = self.client.query(traces, "SELECT count(1) as count FROM slice")

  def test_invalid_bucket(self):
    with self.assertRaises(PerfettoException):
      traces = [f"/gcs//o/{TRACE_PATH}"]
      result = self.client.query(traces, "SELECT count(1) as count FROM slice")
