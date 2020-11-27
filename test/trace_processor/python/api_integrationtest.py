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

import os
import unittest

from trace_processor.api import TraceProcessor


class TestApi(unittest.TestCase):

  def test_trace_file(self):
    # Get path to trace_processor_shell and construct TraceProcessor
    tp = TraceProcessor(
        file_path=os.path.join(os.environ["ROOT_DIR"], 'test', 'data',
                               'example_android_trace_30s.pb'),
        bin_path=os.environ["SHELL_PATH"])
    qr_iterator = tp.query('select * from slice limit 10')
    dur_result = [
        178646, 119740, 58073, 155000, 173177, 20209377, 3589167, 90104, 275312,
        65313
    ]

    for num, row in enumerate(qr_iterator):
      self.assertEqual(row.type, 'internal_slice')
      self.assertEqual(row.dur, dur_result[num])

    # Test the batching logic by issuing a large query and ensuring we receive
    # all rows, not just a truncated subset.
    qr_iterator = tp.query('select count(*) as cnt from slice')
    expected_count = next(qr_iterator).cnt
    self.assertGreater(expected_count, 0)

    qr_iterator = tp.query('select * from slice')
    count = sum(1 for _ in qr_iterator)
    self.assertEqual(count, expected_count)

    tp.close()
