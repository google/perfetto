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

from __future__ import print_function
import time
import subprocess
from os.path import relpath, dirname, join

USE_PYTHON3 = True


def RunAndReportIfLong(func, *args, **kargs):
  start = time.time()
  results = func(*args, **kargs)
  end = time.time()
  limit = 3.0  # seconds
  name = func.__name__
  runtime = end - start
  if runtime > limit:
    print("{} took >{:.2}s ({:.2}s)".format(name, limit, runtime))
  return results


def CheckChange(input, output):
  results = []
  results += RunAndReportIfLong(CheckPrettierAndEslint, input, output)
  results += RunAndReportIfLong(CheckImports, input, output)
  results += RunAndReportIfLong(CheckAnyRachet, input, output)
  return results


def CheckChangeOnUpload(input_api, output_api):
  return CheckChange(input_api, output_api)


def CheckChangeOnCommit(input_api, output_api):
  return CheckChange(input_api, output_api)


def CheckPrettierAndEslint(input_api, output_api):
  ui_path = input_api.PresubmitLocalPath()
  format_sources_path = join(ui_path, 'format-sources')
  cmd = [format_sources_path, '--check-only']
  if subprocess.call(cmd):
    s = ' '.join(cmd)
    return [
        output_api.PresubmitError(f"""Prettier/Eslint errors. To fix, run:
{format_sources_path}""")
    ]
  return []


def CheckImports(input_api, output_api):
  path = input_api.os_path
  ui_path = input_api.PresubmitLocalPath()
  check_imports_path = join(dirname(ui_path), 'tools', 'check_imports')

  def file_filter(x):
    return input_api.FilterSourceFile(
        x, files_to_check=[r'.*\.ts$', r'.*\.js$'])

  files = input_api.AffectedSourceFiles(file_filter)

  if not files:
    return []

  if subprocess.call([check_imports_path]):
    return [output_api.PresubmitError(f"")]
  return []


def CheckAnyRachet(input_api, output_api):
  path = input_api.os_path
  ui_path = input_api.PresubmitLocalPath()
  check_ratchet_path = join(dirname(ui_path), 'tools', 'check_ratchet')

  def file_filter(x):
    return input_api.FilterSourceFile(x, files_to_check=[r'.*\.ts$'])

  files = input_api.AffectedSourceFiles(file_filter)

  if not files:
    return []

  if subprocess.call([check_ratchet_path]):
    return [output_api.PresubmitError(f"")]
  return []
