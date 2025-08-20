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

import os
import signal
import sys
from typing import Any, Dict, IO, List

from google.protobuf import descriptor_pb2, message_factory
from python.generators.diff_tests import models


class ProtoManager:
  """A helper class for all proto related tasks."""

  def __init__(self, descriptor_file_paths: List[str]):
    files = []
    for file_path in descriptor_file_paths:
      files.extend(self.read_descriptor(file_path).file)
    self.messages = message_factory.GetMessages(files)

  def read_descriptor(self, file_name: str) -> descriptor_pb2.FileDescriptorSet:
    """Reads a file descriptor set from a file."""
    with open(file_name, 'rb') as f:
      contents = f.read()

    descriptor = descriptor_pb2.FileDescriptorSet()
    descriptor.MergeFromString(contents)

    return descriptor

  def create_message(self, proto_type: str) -> Any:
    return self.messages[proto_type]


USE_COLOR_CODES = sys.stderr.isatty()


class ColorFormatter:
  """A helper class for color formatting in the terminal."""

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


def get_env(root_dir: str) -> Dict[str, str]:
  """Returns the environment variables for running trace_processor."""
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


def ctrl_c_handler(_num: int, _frame: Any):
  """Handles Ctrl+C by killing the whole process group."""
  # Send a sigkill to the whole process group. Our process group looks like:
  # - Main python interpreter running the main()
  #   - N python interpreters coming from ProcessPoolExecutor workers.
  #     - 1 trace_processor_shell subprocess from subprocess.Popen().
  # We don't need any graceful termination as the diff tests are stateless and
  # don't write any file. Just kill them all immediately.
  os.killpg(os.getpid(), signal.SIGKILL)


def serialize_textproto_trace(trace_descriptor_path: str,
                              extension_descriptor_paths: List[str],
                              text_proto_path: str, out_stream: IO[bytes]):
  from python.generators.diff_tests.trace_generator import TraceGenerator
  TraceGenerator(trace_descriptor_path,
                 extension_descriptor_paths).serialize_textproto_trace(
                     text_proto_path, out_stream)


def serialize_python_trace(root_dir: str, trace_descriptor_path: str,
                           extension_descriptor_paths: List[str],
                           python_trace_path: str, out_stream: IO[bytes]):
  from python.generators.diff_tests.trace_generator import TraceGenerator
  TraceGenerator(trace_descriptor_path,
                 extension_descriptor_paths).serialize_python_trace(
                     root_dir, python_trace_path, out_stream)


def get_trace_descriptor_path(out_path: str, trace_descriptor: str):
  """Returns the path to the trace descriptor."""
  if trace_descriptor:
    return trace_descriptor

  path = ['gen', 'protos', 'perfetto', 'trace', 'trace.descriptor']
  trace_descriptor_path = os.path.join(out_path, *path)
  if not os.path.exists(trace_descriptor_path):
    trace_descriptor_path = os.path.join(out_path, 'gcc_like_host', *path)

  return trace_descriptor_path


def read_all_tests(name_filter: str,
                   root_dir: os.PathLike) -> List[models.TestCase]:
  """Reads all diff tests from the given directory."""
  from python.generators.diff_tests.test_loader import TestLoader
  return TestLoader(root_dir).discover_and_load_tests(name_filter)


def write_diff(expected: str, actual: str) -> str:
  """Returns a unified diff of the two strings."""
  import difflib
  expected_lines = expected.splitlines(True)
  actual_lines = actual.splitlines(True)
  diff = difflib.unified_diff(
      expected_lines, actual_lines, fromfile='expected', tofile='actual')
  return "".join(list(diff))
