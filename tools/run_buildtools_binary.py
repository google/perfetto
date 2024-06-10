#!/usr/bin/env python3
# Copyright (C) 2021 The Android Open Source Project
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
""" A wrapper to run gn, ninja and other buildtools/ for all platforms. """

from __future__ import print_function

import os
import subprocess
import sys

from platform import system

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run_buildtools_binary(args):
  if len(args) < 1:
    print('Usage %s command [args]\n' % sys.argv[0])
    return 1

  sys_name = system().lower()
  os_dir = None
  ext = ''
  if sys_name == 'windows':
    os_dir = 'win'
    ext = '.exe'
  elif sys_name == 'darwin':
    os_dir = 'mac'
  elif sys_name == 'linux':
    os_dir = 'linux64'
  else:
    print('OS not supported: %s\n' % sys_name)
    return 1

  cmd = args[0]
  args = args[1:]

  # Some binaries have been migrated to third_party/xxx. Look into that path
  # first (see b/261398524)
  exe_path = os.path.join(ROOT_DIR, 'third_party', cmd, cmd) + ext
  if not os.path.exists(exe_path):
    exe_path = os.path.join(ROOT_DIR, 'buildtools', os_dir, cmd) + ext

  if sys_name == 'windows':
    # execl() behaves oddly on Windows: the spawned process doesn't seem to
    # receive CTRL+C. Use subprocess instead.
    sys.exit(subprocess.call([exe_path] + args))
  else:
    os.execl(exe_path, os.path.basename(exe_path), *args)


if __name__ == '__main__':
  run_buildtools_binary(sys.argv[1:])
