#!/usr/bin/env python3
# Copyright (C) 2022 The Android Open Source Project
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

# This tool checks that every create (table|view) without prefix
# 'internal_' is documented with proper schema.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import os
import re
import sys

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def check(path):

  # Get module name
  module_name = path.split('/stdlib/')[-1].split('/')[0]

  with open(path) as f:
    lines = [l.strip() for l in f.readlines()]

  # Check that CREATE VIEW/TABLE has a matching schema before it.
  errors = 0
  obj_type, obj_name, schema_cols, schema_desc = None, None, False, False
  for i in range(len(lines)):
    m = re.match(
        r'^CREATE (?:VIRTUAL )?(TABLE|VIEW)?'
        r'(?:IF NOT EXISTS)? (.*) (?:AS|USING).*', lines[i])

    # Ignore all lines that don't create an object
    if m is None:
      continue

    obj_name = m.group(2)

    # Ignore 'internal_' tables|views
    if re.match(r'^internal_.*', obj_name):
      continue

    # Check whether the name starts with module_name
    if not re.match(f'^{module_name}_.*', obj_name):
      sys.stderr.write(f"Invalid name in module {obj_name}. "
                       f"View/table name has to begin with {module_name}_.\n")
      sys.stderr.write(('%s:\n"%s"\n') % (path, lines[i]))
      errors += 1

    # Validate the schema before the create line.
    lines_over_create = lines[i - 1::-1]
    for line in lines_over_create:
      # Ignore empty lines, or only '--' line.
      if not line or line == '--':
        continue

      # Break on SQL lines (lines with words without '--' at the beginning)
      if not line.startswith('--'):
        break

      # Look for '-- @column' line as a column description
      m = re.match(r'^-- @column[ \t]+(\w+)[ \t]+(.*)', line)
      if m is not None:
        schema_cols = True
        continue

      # The only  option left is a description, but it has to be after
      # schema columns.
      if schema_cols:
        schema_desc = True

    if not schema_cols or not schema_desc:
      sys.stderr.write((f"Missing documentation schema for {obj_name}\n"))
      sys.stderr.write(('%s:\n"%s"\n') % (path, lines[i]))
      errors += 1
    d_type, d_name, schema_cols, schema_desc = None, None, False, False

  return errors


def main():
  errors = 0
  metrics_sources = os.path.join(ROOT_DIR, 'src', 'trace_processor', 'stdlib')
  for root, _, files in os.walk(metrics_sources, topdown=True):
    for f in files:
      path = os.path.join(root, f)
      if path.endswith('.sql'):
        errors += check(path)
  return 0 if errors == 0 else 1


if __name__ == '__main__':
  sys.exit(main())
