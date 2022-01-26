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

# This tool checks that every create (table|view) is prefixed by
# drop (table|view).

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import re
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def check(path):
  with open(path) as f:
    lines = [l.strip() for l in f.readlines()]

  # Check that CREATE VIEW/TABLE has a matching DROP VIEW/TABLE before it.
  errors = 0
  d_type, d_name = None, None
  for line in lines:
    m = re.match(r'^DROP (TABLE|VIEW) IF EXISTS (.*);$', line)
    if m is not None:
      d_type, d_name = m.group(1), m.group(2)
      continue
    m = re.match(r'^CREATE (?:VIRTUAL )?(TABLE|VIEW) (.*) (?:AS|USING).*', line)
    if m is None:
      continue
    type, name = m.group(1), m.group(2)
    if type != d_type or name != d_name:
      sys.stderr.write(
          ('Missing DROP %s before CREATE %s\n') % (d_type, d_type))
      sys.stderr.write(('%s:\n"%s" vs %s %s\n') % (path, line, d_type, d_name))
      errors += 1
    d_type, d_name = None, None

  # Ban the use of LIKE in non-comment lines.
  for line in lines:
    if line.startswith('--'):
      continue

    if 'like' in line.casefold():
      sys.stderr.write(
          'LIKE is banned in trace processor metrics. Prefer GLOB instead.\n')
      sys.stderr.write('Offending file: %s\n' % path)
      errors += 1

  return errors


def main():
  errors = 0
  metrics_sources = os.path.join(ROOT_DIR, 'src', 'trace_processor', 'metrics',
                                 'sql')
  for root, _, files in os.walk(metrics_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if path.endswith('.sql'):
        errors += check(path)
  return 0 if errors == 0 else 1


if __name__ == '__main__':
  sys.exit(main())
