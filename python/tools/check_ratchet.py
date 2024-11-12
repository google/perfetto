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
"""
Force the reduction in use of some methods/types over time.
Often a method ('LEGACY_registerTrackController') or a type ('any')
gets replaced by a better alternative ('registerTrack', 'unknown') and
we want to a. replace all existing uses, b. prevent the introduction of
new uses. This presubmit helps with both. It keeps a count of the
number of instances of "FOO" in the codebase. At presubmit time we run
the script. If the "FOO" count has gone up we encourage the author to
use the alternative. If the "FOO" count has gone down we congratulate
them and prompt them to reduce the expected count.
Since the number of "FOO"s can only go down eventually they will all
be gone - completing the migration.
See also https://qntm.org/ratchet.
"""

import sys
import os
import re
import argparse
import collections
import dataclasses

from dataclasses import dataclass

EXPECTED_ANY_COUNT = 52
EXPECTED_RUN_METRIC_COUNT = 4

ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UI_SRC_DIR = os.path.join(ROOT_DIR, 'ui', 'src')


@dataclasses.dataclass
class Check:
  regex: str
  expected_count: int
  expected_variable_name: str
  description: str


CHECKS = [
    # 'any' is too generic. It will show up in many comments etc. So
    # instead of counting any directly we forbid it using eslint and count
    # the number of suppressions.
    Check(r"// eslint-disable-next-line @typescript-eslint/no-explicit-any",
          EXPECTED_ANY_COUNT, "EXPECTED_ANY_COUNT",
          "We should avoid using any whenever possible. Prefer unknown."),
    Check(
        r"RUN_METRIC\(", EXPECTED_RUN_METRIC_COUNT, "EXPECTED_RUN_METRIC_COUNT",
        "RUN_METRIC() is not a stable trace_processor API. Use a stdlib function or macro. See https://perfetto.dev/docs/analysis/perfetto-sql-syntax#defining-functions."
    ),
]


def all_source_files():
  for root, dirs, files in os.walk(UI_SRC_DIR, followlinks=False):
    for name in files:
      if name.endswith('.ts'):
        yield os.path.join(root, name)


def do_check(options):
  c = collections.Counter()

  for path in all_source_files():
    with open(path) as f:
      s = f.read()
    for check in CHECKS:
      count = len(re.findall(check.regex, s))
      c[check.expected_variable_name] += count

  for check in CHECKS:
    actual_count = c[check.expected_variable_name]

    if actual_count > check.expected_count:
      print(f'More "{check.regex}" {check.expected_count} -> {actual_count}')
      print(
          f'  Expected to find {check.expected_count} instances of "{check.regex}" accross the .ts & .d.ts files in the code base.'
      )
      print(f'  Instead found {actual_count}.')
      print(
          f'  It it likely your CL introduces additional uses of "{check.regex}".'
      )
      print(f'  {check.description}')
      return 1
    elif actual_count < check.expected_count:
      print(f'Less "{check.regex}" {check.expected_count} -> {actual_count}')
      print(
          f'  Congratulations your CL reduces the instances of "{check.regex}" in the code base from {check.expected_count} to {actual_count}.'
      )
      print(
          f'  Please go to {__file__} and set {check.expected_variable_name} to {actual_count}.'
      )
      return 1

  return 0


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.set_defaults(func=do_check)
  subparsers = parser.add_subparsers()

  check_command = subparsers.add_parser(
      'check', help='Check the rules (default)')
  check_command.set_defaults(func=do_check)

  options = parser.parse_args()
  return options.func(options)


if __name__ == '__main__':
  sys.exit(main())
