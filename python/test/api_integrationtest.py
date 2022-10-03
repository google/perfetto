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

import io
import os
import unittest
from typing import Optional

import pandas as pd

from perfetto.batch_trace_processor.api import BatchTraceProcessor
from perfetto.batch_trace_processor.api import BatchTraceProcessorConfig
from perfetto.batch_trace_processor.api import FailureHandling
from perfetto.batch_trace_processor.api import Metadata
from perfetto.batch_trace_processor.api import TraceListReference
from perfetto.trace_processor.api import PLATFORM_DELEGATE
from perfetto.trace_processor.api import TraceProcessor
from perfetto.trace_processor.api import TraceProcessorException
from perfetto.trace_processor.api import TraceProcessorConfig
from perfetto.trace_processor.api import TraceReference
from perfetto.trace_uri_resolver.resolver import TraceUriResolver
from perfetto.trace_uri_resolver.path import PathUriResolver


class SimpleResolver(TraceUriResolver):
  PREFIX = 'simple'

  def __init__(self, path, skip_resolve_file=False):
    self.path = path
    self.file = open(example_android_trace_path(), 'rb')
    self.skip_resolve_file = skip_resolve_file

  def file_gen(self):
    with open(example_android_trace_path(), 'rb') as f:
      yield f.read()

  def resolve(self):
    res = [
        TraceUriResolver.Result(
            self.file_gen(), metadata={'source': 'generator'}),
        TraceUriResolver.Result(
            example_android_trace_path(), metadata={'source': 'path'}),
    ]
    if not self.skip_resolve_file:
      res.extend([
          TraceUriResolver.Result(
              PathUriResolver(example_android_trace_path()),
              metadata={'source': 'path_resolver'}),
          TraceUriResolver.Result(self.file, metadata={'source': 'file'}),
      ])
    return res


class RecursiveResolver(SimpleResolver):
  PREFIX = 'recursive'

  def __init__(self, path, skip_resolve_file):
    super().__init__(path=path, skip_resolve_file=skip_resolve_file)

  def resolve(self):
    srf = self.skip_resolve_file
    return [
        TraceUriResolver.Result(
            self.file_gen(), metadata={'source': 'recursive_gen'}),
        TraceUriResolver.Result(
            f'simple:path={self.path};skip_resolve_file={srf}',
            metadata={
                'source': 'recursive_path',
                'root_source': 'recursive_path'
            }),
        TraceUriResolver.Result(
            SimpleResolver(
                path=self.path, skip_resolve_file=self.skip_resolve_file),
            metadata={
                'source': 'recursive_obj',
                'root_source': 'recursive_obj'
            }),
    ]


class SimpleObserver(BatchTraceProcessor.Observer):

  def __init__(self):
    self.execution_times = []

  def trace_processed(self, metadata: Metadata, execution_time_seconds: float):
    self.execution_times.append(execution_time_seconds)


def create_batch_tp(
    traces: TraceListReference,
    load_failure_handling: FailureHandling = FailureHandling.RAISE_EXCEPTION,
    execute_failure_handling: FailureHandling = FailureHandling.RAISE_EXCEPTION,
    observer: Optional[BatchTraceProcessor.Observer] = None):
  registry = PLATFORM_DELEGATE().default_resolver_registry()
  registry.register(SimpleResolver)
  registry.register(RecursiveResolver)
  config = BatchTraceProcessorConfig(
      load_failure_handling=load_failure_handling,
      execute_failure_handling=execute_failure_handling,
      tp_config=TraceProcessorConfig(
          bin_path=os.environ["SHELL_PATH"], resolver_registry=registry))
  return BatchTraceProcessor(traces=traces, config=config, observer=observer)


def create_tp(trace: TraceReference):
  return TraceProcessor(
      trace=trace,
      config=TraceProcessorConfig(bin_path=os.environ["SHELL_PATH"]))


def example_android_trace_path():
  return os.path.join(os.environ["ROOT_DIR"], 'test', 'data',
                      'example_android_trace_30s.pb')


class TestApi(unittest.TestCase):

  def test_invalid_trace(self):
    f = io.BytesIO(b'<foo></foo>')
    with self.assertRaises(TraceProcessorException):
      _ = create_tp(trace=f)

  def test_trace_path(self):
    # Get path to trace_processor_shell and construct TraceProcessor
    tp = create_tp(trace=example_android_trace_path())
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

  def test_trace_byteio(self):
    f = io.BytesIO(
        b'\n(\n&\x08\x00\x12\x12\x08\x01\x10\xc8\x01\x1a\x0b\x12\t'
        b'B|200|foo\x12\x0e\x08\x02\x10\xc8\x01\x1a\x07\x12\x05E|200')
    with create_tp(trace=f) as tp:
      qr_iterator = tp.query('select * from slice limit 10')
      res = list(qr_iterator)

      self.assertEqual(len(res), 1)

      row = res[0]
      self.assertEqual(row.ts, 1)
      self.assertEqual(row.dur, 1)
      self.assertEqual(row.name, 'foo')

  def test_trace_file(self):
    with open(example_android_trace_path(), 'rb') as file:
      with create_tp(trace=file) as tp:
        qr_iterator = tp.query('select * from slice limit 10')
        dur_result = [
            178646, 119740, 58073, 155000, 173177, 20209377, 3589167, 90104,
            275312, 65313
        ]

        for num, row in enumerate(qr_iterator):
          self.assertEqual(row.dur, dur_result[num])

  def test_trace_generator(self):

    def reader_generator():
      with open(example_android_trace_path(), 'rb') as file:
        yield file.read(1024)

    with create_tp(trace=reader_generator()) as tp:
      qr_iterator = tp.query('select * from slice limit 10')
      dur_result = [
          178646, 119740, 58073, 155000, 173177, 20209377, 3589167, 90104,
          275312, 65313
      ]

      for num, row in enumerate(qr_iterator):
        self.assertEqual(row.dur, dur_result[num])

  def test_simple_resolver(self):
    dur = [178646, 178646, 178646, 178646]
    source = ['generator', 'path', 'path_resolver', 'file']
    expected = pd.DataFrame(list(zip(dur, source)), columns=['dur', 'source'])

    with create_batch_tp(
        traces='simple:path={}'.format(example_android_trace_path())) as btp:
      df = btp.query_and_flatten('select dur from slice limit 1')
      pd.testing.assert_frame_equal(df, expected, check_dtype=False)

    with create_batch_tp(
        traces=SimpleResolver(path=example_android_trace_path())) as btp:
      df = btp.query_and_flatten('select dur from slice limit 1')
      pd.testing.assert_frame_equal(df, expected, check_dtype=False)

  def test_query_timing(self):
    observer = SimpleObserver()
    with create_batch_tp(
        traces='simple:path={}'.format(example_android_trace_path()),
        observer=observer) as btp:
      btp.query_and_flatten('select dur from slice limit 1')
      self.assertTrue(
          all([x > 0 for x in observer.execution_times]),
          'Running time should be positive')

  def test_recursive_resolver(self):
    dur = [
        178646, 178646, 178646, 178646, 178646, 178646, 178646, 178646, 178646
    ]
    source = ['recursive_gen', 'generator', 'path', 'generator', 'path']
    root_source = [
        None, 'recursive_path', 'recursive_path', 'recursive_obj',
        'recursive_obj'
    ]
    expected = pd.DataFrame(
        list(zip(dur, source, root_source)),
        columns=['dur', 'source', 'root_source'])

    uri = 'recursive:path={};skip_resolve_file=true'.format(
        example_android_trace_path())
    with create_batch_tp(traces=uri) as btp:
      df = btp.query_and_flatten('select dur from slice limit 1')
      pd.testing.assert_frame_equal(df, expected, check_dtype=False)

    with create_batch_tp(
        traces=RecursiveResolver(
            path=example_android_trace_path(), skip_resolve_file=True)) as btp:
      df = btp.query_and_flatten('select dur from slice limit 1')
      pd.testing.assert_frame_equal(df, expected, check_dtype=False)

  def test_btp_load_failure(self):
    f = io.BytesIO(b'<foo></foo>')
    with self.assertRaises(TraceProcessorException):
      _ = create_batch_tp(traces=f)

  def test_btp_load_failure_increment_stat(self):
    f = io.BytesIO(b'<foo></foo>')
    btp = create_batch_tp(
        traces=f, load_failure_handling=FailureHandling.INCREMENT_STAT)
    self.assertEqual(btp.stats().load_failures, 1)

  def test_btp_query_failure(self):
    btp = create_batch_tp(traces=example_android_trace_path())
    with self.assertRaises(TraceProcessorException):
      _ = btp.query('select * from sl')

  def test_btp_query_failure_increment_stat(self):
    btp = create_batch_tp(
        traces=example_android_trace_path(),
        execute_failure_handling=FailureHandling.INCREMENT_STAT)
    _ = btp.query('select * from sl')
    self.assertEqual(btp.stats().execute_failures, 1)

  def test_btp_query_failure_message(self):
    btp = create_batch_tp(
        traces='simple:path={}'.format(example_android_trace_path()))
    with self.assertRaisesRegex(
        TraceProcessorException, expected_regex='.*source.*generator.*'):
      _ = btp.query('select * from sl')
