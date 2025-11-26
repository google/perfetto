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
import tempfile
import unittest
from typing import Optional

import pandas as pd

from perfetto.batch_trace_processor.api import BatchTraceProcessor
from perfetto.batch_trace_processor.api import BatchTraceProcessorConfig
from perfetto.batch_trace_processor.api import FailureHandling
from perfetto.batch_trace_processor.api import Metadata
from perfetto.batch_trace_processor.api import TraceListReference
from perfetto.trace_processor.protos import ProtoFactory
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

  def test_runtime_error(self):
    # We emulate a situation when TP returns an error by passing the --version
    # flag. This makes TP output version information and exit, instead of
    # starting an http server.
    config = TraceProcessorConfig(
        bin_path=os.environ["SHELL_PATH"], extra_flags=["--version"])
    with self.assertRaisesRegex(
        TraceProcessorException,
        expected_regex='.*Trace Processor RPC API version:.*'):
      TraceProcessor(trace=io.BytesIO(b''), config=config)

  def test_trace_path(self):
    # Get path to trace_processor_shell and construct TraceProcessor
    tp = create_tp(trace=example_android_trace_path())
    qr_iterator = tp.query('select * from slice limit 10')
    dur_result = [
        178646, 119740, 58073, 155000, 173177, 20209377, 3589167, 90104, 275312,
        65313
    ]

    for num, row in enumerate(qr_iterator):
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
    sample_path = example_android_trace_path()
    dur = [178646, 178646, 178646, 178646]
    source = ['generator', 'path', 'path_resolver', 'file']

    # Only path and path_resolver will resolve to PathUriResolver so those will have the _path added
    # to their metadata
    path = [None, sample_path, sample_path, None]

    expected = pd.DataFrame(
        list(zip(dur, source, path)), columns=['dur', 'source', '_path'])

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
        float('nan'), 'recursive_path', 'recursive_path', 'recursive_obj',
        'recursive_obj'
    ]
    sample_path = example_android_trace_path()
    path = [None, None, sample_path, None, sample_path]
    expected = pd.DataFrame(
        list(zip(dur, source, root_source, path)),
        columns=['dur', 'source', 'root_source', '_path'])

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

  def test_extra_flags(self):
    with tempfile.TemporaryDirectory() as temp_dir:
      test_package_dir = os.path.join(temp_dir, 'ext')
      os.makedirs(test_package_dir)
      test_module = os.path.join(test_package_dir, 'module.sql')
      with open(test_module, 'w') as f:
        f.write('CREATE TABLE test_table AS SELECT 123 AS test_value\n')
      config = TraceProcessorConfig(
          bin_path=os.environ["SHELL_PATH"],
          extra_flags=['--add-sql-package', test_package_dir])
      with TraceProcessor(trace=io.BytesIO(b''), config=config) as tp:
        qr_iterator = tp.query(
            'SELECT IMPORT("ext.module"); SELECT test_value FROM test_table')
        self.assertEqual(next(qr_iterator).test_value, 123)

  def test_add_sql_packages(self):
    with tempfile.TemporaryDirectory() as temp_dir:
      # Create a directory structure for the package. The root of the
      # package is |temp_dir| and we are creating the file foo/bar.sql
      # inside it.
      test_package_dir = os.path.join(temp_dir, 'foo')
      os.makedirs(test_package_dir)
      test_module = os.path.join(test_package_dir, 'bar.sql')
      with open(test_module, 'w') as f:
        f.write(
            'CREATE PERFETTO TABLE test_sql_module_foo AS SELECT 1 AS value;\n')

      # Create another directory to add to the package to test that multiple
      # packages can be added.
      test_package_dir_2 = os.path.join(temp_dir, 'baz')
      os.makedirs(test_package_dir_2)
      test_module_2 = os.path.join(test_package_dir_2, 'qux.sql')
      with open(test_module_2, 'w') as f:
        f.write(
            'CREATE PERFETTO TABLE test_sql_module_foo_2 AS SELECT 2 AS value;\n'
        )

      config = TraceProcessorConfig(
          bin_path=os.environ["SHELL_PATH"],
          add_sql_packages=[test_package_dir, test_package_dir_2],
      )
      with TraceProcessor(trace=io.BytesIO(b''), config=config) as tp:
        qr_iterator = tp.query('INCLUDE PERFETTO MODULE foo.bar; '
                               'INCLUDE PERFETTO MODULE baz.qux; '
                               'SELECT value FROM test_sql_module_foo')
        self.assertEqual(next(qr_iterator).value, 1)

  def test_trace_summary_failure(self):
    tp = create_tp(trace=example_android_trace_path())
    with self.assertRaises(TraceProcessorException):
      _ = tp.trace_summary(['foo'], ['bar, baz'])
    tp.close()

  def test_trace_summary_success(self):
    metric_spec = """metric_spec: {
        id: "memory_per_process"
        value: "dur"
        query: {
          simple_slices {
            process_name_glob: "ab*"
          }
        }
      }
      """

    tp = create_tp(trace=example_android_trace_path())
    trace_summary = tp.trace_summary([metric_spec], ['memory_per_process'])
    self.assertEqual(trace_summary.metric_bundles[0].specs[0].id,
                     'memory_per_process')
    tp.close()

  def test_trace_summary_success_multiple_metrics(self):
    metric_spec_1 = """metric_spec: {
        id: "metric_one"
        value: "dur"
        query: {
          simple_slices {
            process_name_glob: "ab*"
          }
        }
      }
      """
    metric_spec_2 = """metric_spec: {
        id: "metric_two"
        value: "ts"
        query: {
          simple_slices {
            process_name_glob: "cd*"
          }
        }
      }
      """
    tp = create_tp(trace=example_android_trace_path())
    trace_summary = tp.trace_summary([metric_spec_1, metric_spec_2],
                                     ['metric_one', 'metric_two'])
    self.assertEqual(len(trace_summary.metric_bundles), 2)
    self.assertIn(trace_summary.metric_bundles[0].specs[0].id,
                  ['metric_one', 'metric_two'])
    self.assertIn(trace_summary.metric_bundles[1].specs[0].id,
                  ['metric_one', 'metric_two'])
    tp.close()

  def test_trace_summary_success_with_metadata_query(self):
    metric_spec = """metric_spec: {
        id: "memory_per_process"
        value: "dur"
        query: {
          simple_slices {
            process_name_glob: "ab*"
          }
        }
      }
      query: {
        id: "metadata_query"
        sql {
          sql: "SELECT \'foo\' AS key,  \'bar\' AS value"
          column_names: "key"
          column_names: "value"
        }
      }
      """
    tp = create_tp(trace=example_android_trace_path())
    trace_summary = tp.trace_summary([metric_spec], ['memory_per_process'],
                                     metadata_query_id='metadata_query')
    self.assertEqual(trace_summary.metric_bundles[0].specs[0].id,
                     'memory_per_process')
    self.assertTrue(hasattr(trace_summary, 'metadata'))
    tp.close()

  def test_trace_summary_dont_execute(self):
    metric_spec = """metric_spec: {
        id: "memory_per_process"
        value: "dur"
        query: {
          simple_slices {
            process_name_glob: "ab*"
          }
        }
      }
      """

    tp = create_tp(trace=example_android_trace_path())
    trace_summary = tp.trace_summary([metric_spec], [])
    self.assertEqual(len(trace_summary.metric_bundles), 0)
    tp.close()

  def test_trace_summary_no_ids_specified(self):
    metric_spec_1 = """metric_spec: {
        id: "metric_one"
        value: "dur"
        query: {
          simple_slices {
            process_name_glob: "ab*"
          }
        }
      }
      """
    metric_spec_2 = """metric_spec: {
        id: "metric_two"
        value: "ts"
        query: {
          simple_slices {
            process_name_glob: "cd*"
          }
        }
      }
      """
    tp = create_tp(trace=example_android_trace_path())
    trace_summary = tp.trace_summary([metric_spec_1, metric_spec_2])
    self.assertEqual(len(trace_summary.metric_bundles), 2)
    self.assertIn(trace_summary.metric_bundles[0].specs[0].id,
                  ['metric_one', 'metric_two'])
    self.assertIn(trace_summary.metric_bundles[1].specs[0].id,
                  ['metric_one', 'metric_two'])
    tp.close()

  def test_trace_summary_specs_as_bytes(self):
    platform_delegate = PLATFORM_DELEGATE()
    protos = ProtoFactory(platform_delegate)

    metric_spec_1 = protos.TraceSummarySpec()
    metric_1 = protos.TraceMetricV2Spec()
    metric_1.id = 'metric_one'
    metric_1.value = 'dur'
    metric_1.query.simple_slices.process_name_glob = 'ab*'
    metric_spec_1.metric_spec.extend([metric_1])
    metric_spec_1_bytes = metric_spec_1.SerializeToString()

    metric_spec_2 = protos.TraceSummarySpec()
    metric_2 = protos.TraceMetricV2Spec()
    metric_2.id = 'metric_two'
    metric_2.value = 'ts'
    metric_2.query.simple_slices.process_name_glob = 'cd*'
    metric_spec_2.metric_spec.extend([metric_2])
    metric_spec_2_bytes = metric_spec_2.SerializeToString()

    tp = create_tp(trace=example_android_trace_path())
    trace_summary = tp.trace_summary([metric_spec_1_bytes, metric_spec_2_bytes])
    self.assertEqual(len(trace_summary.metric_bundles), 2)
    self.assertIn(trace_summary.metric_bundles[0].specs[0].id,
                  ['metric_one', 'metric_two'])
    self.assertIn(trace_summary.metric_bundles[1].specs[0].id,
                  ['metric_one', 'metric_two'])
    tp.close()

  def test_trace_summary_specs_as_bytes_and_text(self):
    platform_delegate = PLATFORM_DELEGATE()
    protos = ProtoFactory(platform_delegate)

    metric_spec_1 = protos.TraceSummarySpec()
    metric_1 = protos.TraceMetricV2Spec()
    metric_1.id = 'metric_one'
    metric_1.value = 'dur'
    metric_1.query.simple_slices.process_name_glob = 'ab*'
    metric_spec_1.metric_spec.extend([metric_1])
    metric_spec_1_bytes = metric_spec_1.SerializeToString()

    metric_spec_2 = """metric_spec: {
        id: "metric_two"
        value: "ts"
        query: {
          simple_slices {
            process_name_glob: "cd*"
          }
        }
      }
      """

    tp = create_tp(trace=example_android_trace_path())
    trace_summary = tp.trace_summary([metric_spec_1_bytes, metric_spec_2])
    self.assertEqual(len(trace_summary.metric_bundles), 2)
    self.assertIn(trace_summary.metric_bundles[0].specs[0].id,
                  ['metric_one', 'metric_two'])
    self.assertIn(trace_summary.metric_bundles[1].specs[0].id,
                  ['metric_one', 'metric_two'])
    tp.close()

  def test_metadata_from_path(self):
    # When loading a trace directly from a path, metadata should be empty
    with create_tp(trace=example_android_trace_path()) as tp:
      self.assertEqual(tp.metadata, {"_path": example_android_trace_path()})

  def test_metadata_from_file(self):
    # When loading a trace from a file object, metadata should be empty
    with open(example_android_trace_path(), 'rb') as file:
      with create_tp(trace=file) as tp:
        self.assertEqual(tp.metadata, {})

  def test_metadata_from_generator(self):
    # When loading a trace from a generator, metadata should be empty
    def reader_generator():
      with open(example_android_trace_path(), 'rb') as file:
        yield file.read(1024)

    with create_tp(trace=reader_generator()) as tp:
      self.assertEqual(tp.metadata, {})

  def test_metadata_from_resolver(self):
    # Test that metadata is captured from a URI resolver
    registry = PLATFORM_DELEGATE().default_resolver_registry()
    registry.register(SimpleResolver)

    # Create a custom resolver that returns a single trace with known metadata
    class MetadataTestResolver(TraceUriResolver):
      PREFIX = 'metadata_test'

      def __init__(self):
        pass

      def resolve(self):
        return [
            TraceUriResolver.Result(
                example_android_trace_path(),
                metadata={
                    'test_key': 'test_value',
                    'trace_id': '12345'
                })
        ]

    registry.register(MetadataTestResolver)

    config = TraceProcessorConfig(
        bin_path=os.environ["SHELL_PATH"], resolver_registry=registry)

    with TraceProcessor(trace='metadata_test:', config=config) as tp:
      self.assertEqual(
          tp.metadata, {
              'test_key': 'test_value',
              'trace_id': '12345',
              '_path': example_android_trace_path()
          })

  def test_metadata_from_resolver_merged(self):
    # Test that metadata is merged when using nested resolvers
    registry = PLATFORM_DELEGATE().default_resolver_registry()

    # Create a two-level resolver to test metadata merging
    class OuterResolver(TraceUriResolver):
      PREFIX = 'outer'

      def __init__(self):
        pass

      def resolve(self):
        return [
            TraceUriResolver.Result(
                'inner:',
                metadata={
                    'outer_key': 'outer_value',
                    'shared_key': 'from_outer'
                })
        ]

    class InnerResolver(TraceUriResolver):
      PREFIX = 'inner'

      def __init__(self):
        pass

      def resolve(self):
        return [
            TraceUriResolver.Result(
                example_android_trace_path(),
                metadata={
                    'inner_key': 'inner_value',
                    'shared_key': 'from_inner'
                })
        ]

    registry.register(OuterResolver)
    registry.register(InnerResolver)

    config = TraceProcessorConfig(
        bin_path=os.environ["SHELL_PATH"], resolver_registry=registry)

    with TraceProcessor(trace='outer:', config=config) as tp:
      # Inner metadata should override outer metadata for shared keys
      expected_metadata = {
          'outer_key': 'outer_value',
          'inner_key': 'inner_value',
          'shared_key': 'from_inner',
          '_path': example_android_trace_path()
      }
      self.assertEqual(tp.metadata, expected_metadata)
