#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

import sys
import os
import signal
from typing import List
import subprocess

from google.protobuf import descriptor_pb2, message_factory, text_format

from python.generators.diff_tests import testing

USE_COLOR_CODES = sys.stderr.isatty()


class ColorFormatter:

  def __init__(self, no_colors: bool):
    self.no_colors = no_colors

  def __red_str(self) -> str:
    return "\u001b[31m" if USE_COLOR_CODES and not self.no_colors else ""

  def __green_str(self) -> str:
    return "\u001b[32m" if USE_COLOR_CODES and not self.no_colors else ""

  def __yellow_str(self) -> str:
    return "\u001b[33m" if USE_COLOR_CODES and not self.no_colors else ""

  def __end_color(self) -> str:
    return "\u001b[0m" if USE_COLOR_CODES and not self.no_colors else ""

  def red(self, s: str) -> str:
    return self.__red_str() + s + self.__end_color()

  def green(self, s: str) -> str:
    return self.__green_str() + s + self.__end_color()

  def yellow(self, s: str) -> str:
    return self.__yellow_str() + s + self.__end_color()


def get_env(root_dir):
  env = {
      'PERFETTO_BINARY_PATH': os.path.join(root_dir, 'test', 'data'),
  }
  if sys.platform.startswith('linux'):
    env['PATH'] = os.path.join(root_dir, 'buildtools', 'linux64', 'clang',
                               'bin')
  elif sys.platform.startswith('darwin'):
    # Sadly, on macOS we need to check out the Android deps to get
    # llvm symbolizer.
    env['PATH'] = os.path.join(root_dir, 'buildtools', 'ndk', 'toolchains',
                               'llvm', 'prebuilt', 'darwin-x86_64', 'bin')
  elif sys.platform.startswith('win32'):
    env['PATH'] = os.path.join(root_dir, 'buildtools', 'win', 'clang', 'bin')
  return env


def ctrl_c_handler(_num, _frame):
  # Send a sigkill to the whole process group. Our process group looks like:
  # - Main python interpreter running the main()
  #   - N python interpreters coming from ProcessPoolExecutor workers.
  #     - 1 trace_processor_shell subprocess coming from the subprocess.Popen().
  # We don't need any graceful termination as the diff tests are stateless and
  # don't write any file. Just kill them all immediately.
  os.killpg(os.getpid(), signal.SIGKILL)


def create_message_factory(descriptor_file_paths, proto_type):
  files = []
  for file_path in descriptor_file_paths:
    files.extend(read_descriptor(file_path).file)

  # We use this method rather than working directly with DescriptorPool
  # because, when the pure-Python protobuf runtime is used, extensions
  # need to be explicitly registered with the message type. See
  # https://github.com/protocolbuffers/protobuf/blob/9e09343a49e9e75be576b31ed7402bf8502b080c/python/google/protobuf/message_factory.py#L145
  return message_factory.GetMessages(files)[proto_type]


def create_metrics_message_factory(metrics_descriptor_paths):
  return create_message_factory(metrics_descriptor_paths,
                                'perfetto.protos.TraceMetrics')


def read_descriptor(file_name):
  with open(file_name, 'rb') as f:
    contents = f.read()

  descriptor = descriptor_pb2.FileDescriptorSet()
  descriptor.MergeFromString(contents)

  return descriptor


def serialize_textproto_trace(trace_descriptor_path, extension_descriptor_paths,
                              text_proto_path, out_stream):
  proto = create_message_factory([trace_descriptor_path] +
                                 extension_descriptor_paths,
                                 'perfetto.protos.Trace')()

  with open(text_proto_path, 'r') as text_proto_file:
    text_format.Merge(text_proto_file.read(), proto)
  out_stream.write(proto.SerializeToString())
  out_stream.flush()


def serialize_python_trace(root_dir, trace_descriptor_path, python_trace_path,
                           out_stream):
  python_cmd = [
      'python3',
      python_trace_path,
      trace_descriptor_path,
  ]

  # Add the test dir to the PYTHONPATH to allow synth_common to be found.
  env = os.environ.copy()
  if 'PYTHONPATH' in env:
    env['PYTHONPATH'] = "{}:{}".format(
        os.path.join(root_dir, 'test'), env['PYTHONPATH'])
  else:
    env['PYTHONPATH'] = os.path.join(root_dir, 'test')
  subprocess.check_call(python_cmd, env=env, stdout=out_stream)


def get_trace_descriptor_path(out_path: str, trace_descriptor: str):
  if trace_descriptor:
    return trace_descriptor

  path = ['gen', 'protos', 'perfetto', 'trace', 'trace.descriptor']
  trace_descriptor_path = os.path.join(out_path, *path)
  if not os.path.exists(trace_descriptor_path):
    trace_descriptor_path = os.path.join(out_path, 'gcc_like_host', *path)

  return trace_descriptor_path


def modify_trace(trace_descriptor_path, extension_descriptor_paths,
                 in_trace_path, out_trace_path, modifier):
  trace_proto = create_message_factory([trace_descriptor_path] +
                                       extension_descriptor_paths,
                                       'perfetto.protos.Trace')()

  with open(in_trace_path, "rb") as f:
    # This may raise DecodeError when |in_trace_path| isn't protobuf.
    trace_proto.ParseFromString(f.read())
    # Modify the trace proto object with the provided modifier function.
    modifier.inject(trace_proto)

  with open(out_trace_path, "wb") as f:
    f.write(trace_proto.SerializeToString())
    f.flush()


def read_all_tests(name_filter: str, root_dir: str) -> List[testing.TestCase]:
  # Import
  INCLUDE_PATH = os.path.join(root_dir, 'test', 'trace_processor', 'diff_tests')
  sys.path.append(INCLUDE_PATH)
  from include_index import fetch_all_diff_tests
  sys.path.pop()
  diff_tests = fetch_all_diff_tests(INCLUDE_PATH)

  return [test for test in diff_tests if test.validate(name_filter)]
