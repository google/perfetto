#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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
"""Extracts a specific vX.Y section from the CHANGELOG to stdout.

Used by the tag-on-stable-push GitHub Actions workflow to
pre-populate the draft release body with the matching CHANGELOG
entry. The body is then expected to be hand-edited into full
release notes before publishing.
"""

import argparse
import os
import re
import sys

PROJECT_ROOT = os.path.abspath(
    os.path.dirname(os.path.dirname(os.path.dirname(__file__))))


def extract(changelog_text, version):
  """Returns the CHANGELOG body for `version` (e.g. 'v54.0').

  The body spans from the line after the header 'vX.Y - DATE:' up to (but
  not including) the next 'vA.B - DATE:' header or end of file. Leading and
  trailing blank lines are trimmed.
  """
  header_re = re.compile(r'^v\d+[.]\d+\s+-\s+\d{4}-\d{2}-\d{2}:\s*$')
  target_re = re.compile(r'^%s\s+-\s+\d{4}-\d{2}-\d{2}:\s*$' %
                         re.escape(version))

  lines = changelog_text.splitlines()
  start = None
  for i, line in enumerate(lines):
    if target_re.match(line):
      start = i + 1
      break
  if start is None:
    raise RuntimeError('Version %s not found in CHANGELOG' % version)

  end = len(lines)
  for i in range(start, len(lines)):
    if header_re.match(lines[i]):
      end = i
      break

  body = lines[start:end]
  while body and not body[0].strip():
    body.pop(0)
  while body and not body[-1].strip():
    body.pop()
  return '\n'.join(body)


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument('version', help='Version to extract, e.g. v54.0')
  parser.add_argument(
      '--changelog',
      default=os.path.join(PROJECT_ROOT, 'CHANGELOG'),
      help='Path to CHANGELOG (default: repo root)')
  args = parser.parse_args()

  with open(args.changelog) as f:
    text = f.read()
  sys.stdout.write(extract(text, args.version))
  sys.stdout.write('\n')


if __name__ == '__main__':
  sys.exit(main())
