#!/usr/bin/env python
# Copyright (C) 2019 The Android Open Source Project
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

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))


def call(cmd, *args):
  path = os.path.join('tools', cmd)
  command = [path] + list(args)
  print 'Running', ' '.join(command)
  try:
    return subprocess.check_output(command, cwd=ROOT_DIR)
  except subprocess.CalledProcessError as e:
    assert False, 'Command: {} failed'.format(' '.join(command))


def check_amalgamated_output():
  call('gen_amalgamated', '--check', '--quiet')


def check_amalgamated_dependencies():
  os_deps = {}
  for os_name in ['android', 'linux', 'mac']:
    os_deps[os_name] = call(
        'gen_amalgamated', '--gn_args', 'target_os="%s"' % os_name,
        '--dump-deps', '--quiet').split('\n')
  for os_name, deps in os_deps.items():
    for dep in deps:
      for other_os, other_deps in os_deps.items():
        if not dep in other_deps:
          raise AssertionError('Discrepancy in amalgamated build dependencies: '
                               '%s is missing on %s.' % (dep, other_os))


def main():
  check_amalgamated_output()
  check_amalgamated_dependencies()


if __name__ == '__main__':
  exit(main())
