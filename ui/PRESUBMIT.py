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
from os.path import relpath


def RunAndReportIfLong(func, *args, **kargs):
  start = time.time()
  results = func(*args, **kargs)
  end = time.time()
  limit = 0.5  # seconds
  name = func.__name__
  runtime = end - start
  if runtime > limit:
    print("{} took >{:.2}s ({:.2}s)".format(name, limit, runtime))
  return results


def CheckChange(input, output):
  results = []
  return results


def CheckChangeOnUpload(input_api, output_api):
  return CheckChange(input_api, output_api)


def CheckChangeOnCommit(input_api, output_api):
  return CheckChange(input_api, output_api)
