#!/usr/bin/env python
# Copyright (C) 2018 The Android Open Source Project
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
import difflib
import glob
import importlib
import os
import subprocess
import sys
import tempfile

from google.protobuf import descriptor, descriptor_pb2, message_factory
from google.protobuf import reflection, text_format
from google.protobuf.pyext import _message

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def create_metrics_message_factory(metrics_descriptor_path):
  with open(metrics_descriptor_path, "r") as metrics_descriptor_file:
    metrics_descriptor_content = metrics_descriptor_file.read()

  file_desc_set_pb2 = descriptor_pb2.FileDescriptorSet()
  file_desc_set_pb2.MergeFromString(metrics_descriptor_content)

  desc_by_path = {}
  for f_desc_pb2 in file_desc_set_pb2.file:
    f_desc_pb2_encode = f_desc_pb2.SerializeToString()
    f_desc = descriptor.FileDescriptor(
        name=f_desc_pb2.name,
        package=f_desc_pb2.package,
        serialized_pb=f_desc_pb2_encode)

    for desc in f_desc.message_types_by_name.values():
      desc_by_path[desc.full_name] = desc

  return message_factory.MessageFactory().GetPrototype(
      desc_by_path["perfetto.protos.TraceMetrics"])

def write_diff(expected, actual):
  expected_lines = expected.splitlines(True)
  actual_lines = actual.splitlines(True)
  diff = difflib.unified_diff(expected_lines, actual_lines,
                              fromfile="expected", tofile="actual")
  for line in diff:
    sys.stderr.write(line)

def run_metrics_test(trace_processor_path, gen_trace_path, trace_path, metric,
                     expected_path, trace_descriptor_path,
                     metrics_message_factory):
  with open(expected_path, "r") as expected_file:
    expected = expected_file.read()

  cmd = [trace_processor_path, '--run-metrics', metric, gen_trace_path]
  actual = subprocess.check_output(cmd)

  # Expected will be in text proto format and we'll need to parse it to a real
  # proto.
  expected_message = metrics_message_factory()
  text_format.Merge(expected, expected_message)

  # Actual will be the raw bytes of the proto and we'll need to parse it into
  # a message.
  actual_message = metrics_message_factory()
  actual_message.ParseFromString(actual)

  # Do an equality check of the python messages
  if expected_message == actual_message:
    return True

  # Write some metadata about the traces.
  sys.stderr.write(
    "Expected did not match actual for trace {} and metric {}\n"
    .format(trace_path, metric))
  sys.stderr.write("Expected file: {}\n".format(expected_path))
  sys.stderr.write("Command line: {}\n".format(' '.join(cmd)))

  # Convert both back to text format and do a diff between the two.
  expected_text = text_format.MessageToString(expected_message)
  actual_text = text_format.MessageToString(actual_message)
  write_diff(expected_text, actual_text)

  return False

def run_query_test(trace_processor_path, gen_trace_path, trace_path,
                   query_path, expected_path, trace_descriptor_path):
  with open(expected_path, "r") as expected_file:
    expected = expected_file.read()

  cmd = [trace_processor_path, '-q', query_path, gen_trace_path]
  actual = subprocess.check_output(cmd).decode("utf-8")

  if expected == actual:
    return True

  # Write some metadata.
  sys.stderr.write(
    "Expected did not match actual for trace {} and query {}\n"
    .format(trace_path, query_path))
  sys.stderr.write("Expected file: {}\n".format(expected_path))
  sys.stderr.write("Command line: {}\n".format(' '.join(cmd)))

  # Write the diff of the two files.
  write_diff(expected, actual)

  return False

def main():
  parser = argparse.ArgumentParser()
  parser.add_argument("--test-type", type=str, default="queries")
  parser.add_argument('--trace-descriptor', type=str)
  parser.add_argument('--metrics-descriptor', type=str)
  parser.add_argument('trace_processor', type=str,
                      help='location of trace processor binary')
  args = parser.parse_args()

  if args.test_type == 'queries':
    index = os.path.join(ROOT_DIR, "test", "trace_processor", "index")
  elif args.test_type == 'metrics':
    index = os.path.join(ROOT_DIR, "test", "metrics", "index")
  else:
    print("Unknown test type {}. Supported: queries, metircs".format(
      args.test_type))
    return 1

  with open(index, 'r') as file:
    index_lines = file.readlines()

  if args.trace_descriptor:
    trace_descriptor_path = args.trace_descriptor
  else:
    out_path = os.path.dirname(args.trace_processor)
    trace_protos_path = os.path.join(out_path, "gen", "protos", "trace")
    trace_descriptor_path = os.path.join(trace_protos_path, "trace.descriptor")

  if args.metrics_descriptor:
    metrics_descriptor_path = args.metrics_descriptor
  else:
    out_path = os.path.dirname(args.trace_processor)
    metrics_protos_path = os.path.join(out_path, "gen", "protos", "perfetto",
                                       "metrics")
    metrics_descriptor_path = os.path.join(metrics_protos_path,
                                           "metrics.descriptor")

  metrics_message_factory = create_metrics_message_factory(
    metrics_descriptor_path)

  test_failure = 0
  index_dir = os.path.dirname(index)
  for line in index_lines:
    stripped = line.strip()
    if stripped.startswith('#'):
      continue
    elif not stripped:
      continue

    [trace_fname, query_fname_or_metric, expected_fname] = stripped.split(' ')

    trace_path = os.path.abspath(os.path.join(index_dir, trace_fname))
    expected_path = os.path.abspath(os.path.join(index_dir, expected_fname))
    if not os.path.exists(trace_path):
      print("Trace file not found {}".format(trace_path))
      return 1
    elif not os.path.exists(expected_path):
      print("Expected file not found {}".format(expected_path))
      return 1

    if trace_path.endswith('.py'):
      gen_trace_file = tempfile.NamedTemporaryFile()
      python_cmd = ["python", trace_path, trace_descriptor_path]
      subprocess.check_call(python_cmd, stdout=gen_trace_file)
      gen_trace_path = os.path.realpath(gen_trace_file.name)
    else:
      gen_trace_file = None
      gen_trace_path = trace_path

    if args.test_type == 'queries':
      query_path = os.path.abspath(
        os.path.join(index_dir, query_fname_or_metric))
      if not os.path.exists(query_path):
        print("Query file not found {}".format(query_path))
        return 1

      success = run_query_test(args.trace_processor, gen_trace_path,
                               trace_path, query_path, expected_path,
                               trace_descriptor_path)
    elif args.test_type == 'metrics':
      success = run_metrics_test(args.trace_processor, gen_trace_path,
                                 trace_path, query_fname_or_metric,
                                 expected_path, trace_descriptor_path,
                                 metrics_message_factory)
    else:
      assert False

    if gen_trace_file:
      gen_trace_file.close()

    if not success:
      test_failure += 1

  if test_failure == 0:
    print("All tests passed successfully")
    return 0
  else:
    print("Total failures: {}".format(test_failure))
    return 1

if __name__ == '__main__':
  sys.exit(main())
