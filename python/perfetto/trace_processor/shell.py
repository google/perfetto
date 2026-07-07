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
import subprocess
import sys
import tempfile
import time
import shutil
from typing import List, Optional, Union
from urllib import request, error

from perfetto.common.exceptions import PerfettoException
from perfetto.trace_processor.platform import PlatformDelegate
from perfetto.trace_processor.process_tree import (create_kill_on_close_job,
                                                   terminate_process_tree)

# Import TYPE_CHECKING to avoid circular imports
from typing import TYPE_CHECKING
if TYPE_CHECKING:
  from perfetto.trace_processor.api import SqlPackage

# Default port that trace_processor_shell runs on
TP_PORT = 9001


def load_shell(
    bin_path: Optional[str],
    unique_port: bool,
    verbose: bool,
    ingest_ftrace_in_raw: bool,
    enable_dev_features: bool,
    platform_delegate: PlatformDelegate,
    load_timeout: int = 30,
    extra_flags: Optional[List[str]] = None,
    add_sql_packages: Optional[List[Union[str, 'SqlPackage']]] = None,
    fetch_latest_trace_processor: bool = False,
):
  addr, port = platform_delegate.get_bind_addr(
      port=0 if unique_port else TP_PORT)
  url = f'{addr}:{str(port)}'

  shell_path = platform_delegate.get_shell_path(
      bin_path=bin_path, fetch_latest=fetch_latest_trace_processor)

  # get Python interpreter path
  if not getattr(sys, 'frozen', False):
    python_executable_path = sys.executable
  else:
    python_executable_path = shutil.which('python')

  if os.name == 'nt' and not shell_path.endswith('.exe'):
    tp_exec = [python_executable_path, shell_path]
  else:
    tp_exec = [shell_path]

  if fetch_latest_trace_processor and bin_path is None:
    # The wrapper script downloads the real binary on first run. Do this
    # here so it does not race the startup timeout below.
    subprocess.check_call(tp_exec + ['--version'], stdout=subprocess.DEVNULL)

  args = ['-D', '--http-port', str(port)]
  if not ingest_ftrace_in_raw:
    args.append('--no-ftrace-raw')

  if enable_dev_features:
    args.append('--dev')

  if add_sql_packages:
    for package in add_sql_packages:
      if isinstance(package, str):
        args.extend(['--add-sql-package', package])
      else:
        # It's a SqlPackage object
        pkg_str = package.path
        if package.package:
          pkg_str += f'@{package.package}'
        args.extend(['--add-sql-package', pkg_str])

  if extra_flags:
    args.extend(extra_flags)

  temp_stdout = tempfile.TemporaryFile()
  temp_stderr = tempfile.TemporaryFile()

  creationflags = 0
  if sys.platform == 'win32':
    creationflags = subprocess.CREATE_NEW_PROCESS_GROUP

  # Set the child up so its whole process tree can be torn down later (see
  # process_tree). On POSIX, start_new_session puts it in its own session/
  # process group; on Windows it is assigned to a Job Object right after.
  p = subprocess.Popen(
      tp_exec + args,
      stdin=subprocess.DEVNULL,
      stdout=temp_stdout,
      stderr=None if verbose else temp_stderr,
      creationflags=creationflags,
      start_new_session=sys.platform != 'win32')
  job_handle = create_kill_on_close_job(p)

  success = False
  for _ in range(load_timeout + 1):
    try:
      if p.poll() is None:
        _ = request.urlretrieve(f'http://{url}/status')
        success = True
      break
    except (error.URLError, ConnectionError):
      time.sleep(1)

  if not success:
    terminate_process_tree(p, job_handle)
    temp_stdout.seek(0)
    stdout = temp_stdout.read().decode("utf-8")
    temp_stderr.seek(0)
    stderr = temp_stderr.read().decode("utf-8")
    temp_stdout.close()
    temp_stderr.close()
    raise PerfettoException("Trace processor failed to start.\n"
                            f"stdout: {stdout}\nstderr: {stderr}\n"
                            "If this is a slow machine or network, try "
                            "increasing load_timeout in TraceProcessorConfig.")

  return url, p, temp_stdout, temp_stderr, job_handle
