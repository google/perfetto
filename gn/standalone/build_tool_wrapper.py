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

""" Wrapper to invoke compiled build tools from the build system.

This is just a workaround for GN assuming that all external scripts are
python sources. It is used to invoke the built protoc compiler.
"""

import os
import sys

def main():
  cmd = sys.argv[1:]
  if not os.path.exists(cmd[0]):
    print >> sys.stderr, 'Cannot find ' + cmd[0]
    return 1
  os.execv(cmd[0], cmd)

if __name__ == '__main__':
  sys.exit(main())
