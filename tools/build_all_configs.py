#!/usr/bin/env python
# Copyright (C) 2017 The Android Open Source Project
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
import os
import subprocess
import sys

MAC_BUILD_CONFIGS = {
  'mac_debug': ['is_clang=true', 'is_debug=true'],
  'mac_release': ['is_clang=true', 'is_debug=false'],
  'mac_asan': ['is_clang=true', 'is_debug=false', 'is_asan=true'],
  'mac_tsan': ['is_clang=true', 'is_debug=false', 'is_tsan=true'],
  'mac_ubsan': ['is_clang=true', 'is_debug=false', 'is_ubsan=true'],
}

ANDROID_BUILD_CONFIGS = {
  'android_debug': ['target_os="android"', 'is_clang=true', 'is_debug=true'],
  'android_release': ['target_os="android"', 'is_clang=true', 'is_debug=false'],
  'android_asan': ['target_os="android"', 'is_clang=true', 'is_debug=false', 'is_asan=true'],
  'android_lsan': ['target_os="android"', 'is_clang=true', 'is_debug=false', 'is_lsan=true'],
}

ANDROID_ARCHS = [ 'arm', 'arm64' ]

LINUX_BUILD_CONFIGS = {
  'linux_gcc_debug': ['is_clang=false', 'is_debug=true'],
  'linux_gcc_release': ['is_clang=false', 'is_debug=false'],
  'linux_clang_debug': ['is_clang=true', 'is_debug=true'],
  'linux_clang_release': ['is_clang=true', 'is_debug=false'],
  'linux_asan': ['is_clang=true', 'is_debug=false', 'is_asan=true'],
  'linux_lsan': ['is_clang=true', 'is_debug=false', 'is_lsan=true'],
  'linux_msan': ['is_clang=true', 'is_debug=false', 'is_msan=true'],
  'linux_tsan': ['is_clang=true', 'is_debug=false', 'is_tsan=true'],
  'linux_ubsan': ['is_clang=true', 'is_debug=false', 'is_ubsan=true'],
}

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--host-only', action='store_true', default=False)
  parser.add_argument('--build', default=None)
  args = parser.parse_args()

  configs = {}
  if not args.host_only:
    for config_name, gn_args in ANDROID_BUILD_CONFIGS.iteritems():
      for arch in ANDROID_ARCHS:
        full_config_name = '%s_%s' % (config_name, arch)
        configs[full_config_name] = gn_args + ['target_cpu="%s"' % arch]

  if sys.platform == 'linux2':
    configs.update(LINUX_BUILD_CONFIGS)
  elif sys.platform == 'darwin':
    configs.update(MAC_BUILD_CONFIGS)
  else:
    assert(False)

  out_base_dir = os.path.join(ROOT_DIR, 'out')
  if not os.path.isdir(out_base_dir):
    os.mkdir(out_base_dir)

  gn = os.path.join(ROOT_DIR, 'tools', 'gn')

  for config_name, gn_args in configs.iteritems():
    print '\n\033[32mBuilding %-20s[%s]\033[0m' % (config_name, ','.join(gn_args))
    out_dir = os.path.join(ROOT_DIR, 'out', config_name)
    if not os.path.isdir(out_dir):
      os.mkdir(out_dir)
    gn_cmd = [gn, 'args', out_dir, '--args=%s' % (' '.join(gn_args)), '--check']
    print ' '.join(gn_cmd)
    subprocess.check_call(gn_cmd, cwd=ROOT_DIR, env={'EDITOR':'true'})
    if args.build:
      ninja = os.path.join(ROOT_DIR, 'tools', 'ninja')
      ninja_cmd = [ninja, '-C', '.', args.build]
      subprocess.check_call(ninja_cmd, cwd=out_dir)


if __name__ == '__main__':
  sys.exit(main())
