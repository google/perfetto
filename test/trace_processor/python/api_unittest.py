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

import unittest

from trace_processor.api import TraceProcessor
from trace_processor.protos import ProtoFactory


class TestQueryResultIterator(unittest.TestCase):
  # The numbers input into cells correspond the the CellType enum values
  # defined under trace_processor.proto
  CELL_VARINT = ProtoFactory().CellsBatch().CELL_VARINT
  CELL_STRING = ProtoFactory().CellsBatch().CELL_STRING

  def test_one_batch(self):
    # Construct expected results
    int_values = [100, 200]
    str_values = ['bar1', 'bar2']

    # Create cells batch to populate QueryResult
    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch.varint_cells.extend(int_values)
    batch.string_cells = "\0".join(str_values)
    batch.is_last_batch = True

    # Get result from api function
    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch])

    # Assert results are as expected
    for num, row in enumerate(qr_iterator):
      self.assertEqual(row.foo_id, str_values[num])
      self.assertEqual(row.foo_num, int_values[num])

  def test_many_batches(self):
    # Construct expected results
    int_values = [100, 200, 300, 400]
    str_values = ['bar1', 'bar2', 'bar3', 'bar4']

    # Create cells batches to populate QueryResult
    batch_1 = ProtoFactory().CellsBatch()
    batch_1.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch_1.varint_cells.extend(int_values[:2])
    batch_1.string_cells = "\0".join(str_values[:2])
    batch_1.is_last_batch = False

    batch_2 = ProtoFactory().CellsBatch()
    batch_2.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch_2.varint_cells.extend(int_values[2:])
    batch_2.string_cells = "\0".join(str_values[2:])
    batch_2.is_last_batch = True

    # Get result from api function
    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch_1, batch_2])

    # Assert results are as expected
    for num, row in enumerate(qr_iterator):
      self.assertEqual(row.foo_id, str_values[num])
      self.assertEqual(row.foo_num, int_values[num])

  def test_empty_batch(self):
    # Create cells batch to populate QueryResult
    batch = ProtoFactory().CellsBatch()
    batch.is_last_batch = True

    # Get result from api function
    qr_iterator = TraceProcessor.QueryResultIterator([], [batch])

    # Assert results are as expected
    for num, row in enumerate(qr_iterator):
      self.assertIsNone(row.foo_id)
      self.assertIsNone(row.foo_num)

  def test_invalid_batch(self):
    # Create cells batch to populate QueryResult
    batch = ProtoFactory().CellsBatch()

    # Get result from api function
    qr_iterator = TraceProcessor.QueryResultIterator([], [batch])

    # Assert results are as expected
    with self.assertRaises(Exception):
      for row in qr_iterator:
        pass

  def test_incorrect_cells_batch(self):
    str_values = ['bar1', 'bar2']

    # Create cells batch to populate QueryResult
    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_STRING,
        TestQueryResultIterator.CELL_VARINT,
        TestQueryResultIterator.CELL_STRING, TestQueryResultIterator.CELL_VARINT
    ])
    batch.string_cells = "\0".join(str_values)
    batch.is_last_batch = True

    # Get result from api function
    qr_iterator = TraceProcessor.QueryResultIterator(['foo_id', 'foo_num'],
                                                     [batch])

    # Assert results are as expected
    with self.assertRaises(Exception):
      for row in qr_iterator:
        pass

  def test_incorrect_columns_batch(self):
    int_values = [100, 200]

    # Create cells batch to populate QueryResult
    batch = ProtoFactory().CellsBatch()
    batch.cells.extend([
        TestQueryResultIterator.CELL_VARINT, TestQueryResultIterator.CELL_VARINT
    ])
    batch.varint_cells.extend(int_values)
    batch.is_last_batch = True

    # Get result from api function
    qr_iterator = TraceProcessor.QueryResultIterator(
        ['foo_id', 'foo_num', 'foo_dur', 'foo_ms'], [batch])

    # Assert results are as expected
    with self.assertRaises(Exception):
      for row in qr_iterator:
        pass
