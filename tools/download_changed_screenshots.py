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
import base64
import json
import os
import io
import re
import zipfile
import urllib.request
from os import path

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def get_artifact_url(run, name):
  return f'https://storage.googleapis.com/perfetto-ci-artifacts/{run}/ui-test-artifacts/{name}'


def main():
  os.chdir(ROOT_DIR)
  parser = argparse.ArgumentParser()
  parser.add_argument(
      'run',
      metavar='RUN',
      help='CI run identifier, e.g. ' +
      '\'20240923144821--cls-3258215-15--ui-clang-x86_64-release\'')
  args = parser.parse_args()
  url = get_artifact_url(args.run, 'index.html')
  with urllib.request.urlopen(url) as resp:
    handle_report(resp.read().decode('utf-8'), args.run)


def sanitize_attachment(name):
  return re.sub('[ _]', '-', name)


def sanitize_test_name(name):
  # Mirrors Playwright's sanitizeForFilePath in
  # playwright-core/lib/server/utils/fileUtils.js, used to derive
  # {testName} in snapshotPathTemplate.
  return re.sub(r'[\x00-\x2C\x2E-\x2F\x3A-\x40\x5B-\x60\x7B-\x7F]+', '-', name)


def handle_report(report: str, run: str):
  m = re.findall(
      r'<script id="playwrightReportBase64"[^>]*>data:application/zip;base64,([^<]+)</script>',
      report)
  bin = base64.b64decode(m[0])
  z = zipfile.ZipFile(io.BytesIO(bin))
  report = json.loads(z.open('report.json').read().decode())
  pngs = {}
  for f in report['files']:
    test_file = f['fileName'].removeprefix('test/')
    for t in f['tests']:
      title = sanitize_test_name(t['title'])
      for r in t['results']:
        for a in r['attachments']:
          png_name = sanitize_attachment(a['name'])
          if not png_name.endswith('-actual.png'):
            continue
          path = 'test/data/ui-screenshots/%s/%s/%s' % (
              test_file, title, png_name.replace('-actual', ''))
          pngs[path] = a['path']

  for local_path, remote_path in pngs.items():
    url = get_artifact_url(run, remote_path)
    print(f'Downloading {local_path} from {url}')
    os.makedirs(os.path.dirname(local_path), exist_ok=True)
    urllib.request.urlretrieve(url, local_path)

  print('Done. Now run:')
  print('./tools/test_data upload  (or status)')


if __name__ == "__main__":
  main()
