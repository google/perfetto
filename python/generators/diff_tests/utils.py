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

from tools.proto_utils import create_message_factory

USE_COLOR_CODES = sys.stderr.isatty()


def red(no_colors):
  return "\u001b[31m" if USE_COLOR_CODES and not no_colors else ""


def green(no_colors):
  return "\u001b[32m" if USE_COLOR_CODES and not no_colors else ""


def yellow(no_colors):
  return "\u001b[33m" if USE_COLOR_CODES and not no_colors else ""


def end_color(no_colors):
  return "\u001b[0m" if USE_COLOR_CODES and not no_colors else ""


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


def create_metrics_message_factory(metrics_descriptor_paths):
  return create_message_factory(metrics_descriptor_paths,
                                'perfetto.protos.TraceMetrics')


def find_trace_descriptor(parent):
  trace_protos_path = os.path.join(parent, 'gen', 'protos', 'perfetto', 'trace')
  return os.path.join(trace_protos_path, 'trace.descriptor')
