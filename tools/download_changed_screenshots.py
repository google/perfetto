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

import argparse
import sys
import urllib.request
from os import path


def get_artifact_url(run, name):
  return f'https://storage.googleapis.com/perfetto-ci-artifacts/{run}/ui-test-artifacts/{name}'


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('run', metavar='RUN', help='CI run identifier')
  args = parser.parse_args()

  with urllib.request.urlopen(get_artifact_url(args.run, 'report.txt')) as resp:
    handle_report(resp.read().decode('utf-8'), args.run)


def handle_report(report: str, run: str):
  for line in report.split('\n'):
    if len(line) == 0:
      continue

    parts = line.split(';')
    if len(parts) != 2:
      print('Erroneous report line!')
      sys.exit(1)

    screenshot_name = parts[0]
    url = get_artifact_url(run, screenshot_name)
    output_path = path.join('test', 'data', 'ui-screenshots', screenshot_name)
    print(f'Downloading {url}')
    urllib.request.urlretrieve(url, output_path)
  print('Done. Now run:')
  print('./tools/test_data upload')


if __name__ == "__main__":
  main()
