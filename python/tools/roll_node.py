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
"""Rolls the pinned Node.js toolchain.

Downloads the official prebuilt Node.js bundle for every platform
Perfetto supports, repackages each into a flat archive, and optionally
uploads them to gs://perfetto and rewrites the pins in tools/install-build-deps.

Usage:
  tools/roll_node 22.14.0            # build tarballs locally, print pins
  tools/roll_node 22.14.0 --upload   # also upload to GCS and patch deps
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INSTALL_BUILD_DEPS = os.path.join(ROOT_DIR, 'tools', 'install-build-deps')

# Upstream Node.js prebuilt URL.
# e.g., https://nodejs.org/dist/v22.14.0/node-v22.14.0-darwin-arm64.tar.gz
BUILD_URL = 'https://nodejs.org/dist/v{version}/node-v{version}-{node_platform}.tar.gz'

GCS_BUCKET = 'gs://perfetto'

# (pin suffix used in GCS object name, upstream node platform name)
PLATFORMS = [
    ('mac', 'darwin-arm64'),
    ('mac-x64', 'darwin-x64'),
    ('linux', 'linux-x64'),
]


def sha256_file(path):
  h = hashlib.sha256()
  with open(path, 'rb') as f:
    for chunk in iter(lambda: f.read(1 << 20), b''):
      h.update(chunk)
  return h.hexdigest()


def build_tarball(suffix, node_platform, version, work_dir):
  """Downloads and repackages one platform bundle. Returns (tgz path, sha256)."""
  url = BUILD_URL.format(version=version, node_platform=node_platform)
  archive = os.path.join(work_dir, 'upstream-%s.tar.gz' % suffix)
  extract = os.path.join(work_dir, 'extract-%s' % suffix)
  tgz = os.path.join(work_dir, 'node-%s-%s.tgz' % (version, suffix))

  print('[%s] downloading %s' % (suffix, url))
  subprocess.check_call(['curl', '-f', '-L', '-#', '-o', archive, url])

  print('[%s] extracting' % suffix)
  if os.path.isdir(extract):
    shutil.rmtree(extract)
  os.makedirs(extract)
  subprocess.check_call(['tar', '-xf', archive, '-C', extract])

  # The upstream archive extracts to node-v<version>-<platform>/.
  # We want to package the contents of this folder directly so that they are at the
  # top-level of the new tgz archive.
  upstream_dir_name = 'node-v%s-%s' % (version, node_platform)
  contents_dir = os.path.join(extract, upstream_dir_name)
  if not os.path.isdir(contents_dir):
    raise SystemExit('Expected directory %s not found in extracted archive' %
                     contents_dir)

  print('[%s] packaging %s' % (suffix, os.path.basename(tgz)))
  subprocess.check_call(['tar', '-C', contents_dir, '-czf', tgz, '.'])

  # Drop the intermediates now to keep peak disk usage to one bundle at a time.
  os.remove(archive)
  shutil.rmtree(extract)

  digest = sha256_file(tgz)
  print('[%s] sha256 %s' % (suffix, digest))
  return tgz, digest


def upload(tgz, version, suffix):
  target = '%s/node-%s-%s.tgz' % (GCS_BUCKET, version, suffix)
  print('uploading %s -> %s' % (os.path.basename(tgz), target))
  subprocess.check_call([
      'gcloud', 'storage', 'cp', '--no-clobber', '--predefined-acl=publicRead',
      tgz, target
  ])


def patch_install_build_deps(version, digests):
  with open(INSTALL_BUILD_DEPS) as f:
    text = f.read()

  # We have three platform entries to patch.
  patterns = {
      'mac':
          re.compile(
              r"(Dependency\(\s*'buildtools/mac/nodejs\.tgz',\s*')[^']+(\s*',\s*')[0-9a-f]{64}(\s*',\s*'darwin',\s*'arm64'\))"
          ),
      'mac-x64':
          re.compile(
              r"(Dependency\(\s*'buildtools/mac/nodejs\.tgz',\s*')[^']+(\s*',\s*')[0-9a-f]{64}(\s*',\s*'darwin',\s*'x64'\))"
          ),
      'linux':
          re.compile(
              r"(Dependency\(\s*'buildtools/linux64/nodejs\.tgz',\s*')[^']+(\s*',\s*')[0-9a-f]{64}(\s*',\s*'linux',\s*'x64'\))"
          ),
  }

  for suffix, pat in patterns.items():
    digest = digests[suffix]
    replacement = r"\g<1>https://storage.googleapis.com/perfetto/node-%s-%s.tgz\g<2>%s\g<3>" % (
        version, suffix, digest)
    text, n = pat.subn(replacement, text)
    if n != 1:
      raise SystemExit(
          'Expected exactly one %s pin in install-build-deps, found %d' %
          (suffix, n))

  with open(INSTALL_BUILD_DEPS, 'w') as f:
    f.write(text)
  print('patched %s' % INSTALL_BUILD_DEPS)


NODE_INDEX_URL = 'https://nodejs.org/dist/index.json'


def resolve_node_version(query):
  query = query.lstrip('v').strip()

  if query == 'latest':
    with urllib.request.urlopen(NODE_INDEX_URL) as f:
      releases = json.load(f)
    return releases[0]['version'].lstrip('v')

  parts = query.split('.')
  if len(parts) == 1 or (len(parts) == 2 and parts[1] in ('x', '*')):
    # E.g. "22" or "22.x" or "22.*"
    major = parts[0].replace('x', '').replace('*', '')
    prefix = 'v' + major + '.'
    with urllib.request.urlopen(NODE_INDEX_URL) as f:
      releases = json.load(f)
    for r in releases:
      v = r['version']
      if v.startswith(prefix):
        return v.lstrip('v')
    raise SystemExit('Could not find any release for version %s' % query)

  if len(parts) == 2:
    # E.g. "22.12" -> "22.12.0"
    return f"{parts[0]}.{parts[1]}.0"

  # E.g. "22.12.1" -> exact
  return query


def main():
  parser = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('version', help='Node.js release version, e.g. 22.14.0')
  parser.add_argument(
      '--upload',
      action='store_true',
      help='upload tarballs to GCS and patch install-build-deps')
  parser.add_argument(
      '--work-dir',
      help='scratch dir for downloads/repackaging (default: a temp dir under '
      'the repo root; needs a few GB free)')
  parser.add_argument(
      '--platforms',
      help='comma-separated subset of: ' + ', '.join(p[0] for p in PLATFORMS))
  parser.add_argument(
      '--keep', action='store_true', help='keep the work dir when done')
  args = parser.parse_args()

  version = resolve_node_version(args.version)
  print('Resolved Node.js version: %s' % version)

  wanted = set(args.platforms.split(',')) if args.platforms else None
  platforms = [p for p in PLATFORMS if wanted is None or p[0] in wanted]

  owns_work_dir = not args.work_dir
  work_dir = args.work_dir or tempfile.mkdtemp(
      prefix='node-roll-', dir=ROOT_DIR)
  os.makedirs(work_dir, exist_ok=True)
  print('work dir: %s' % work_dir)

  cleanup = False
  try:
    digests, tgzs = {}, {}
    for suffix, node_platform in platforms:
      tgz, digest = build_tarball(suffix, node_platform, version, work_dir)
      digests[suffix] = digest
      tgzs[suffix] = tgz

    if args.upload:
      for suffix in tgzs:
        upload(tgzs[suffix], version, suffix)
      patch_install_build_deps(version, digests)
      print('\nDone. Run `tools/install-build-deps` to fetch and verify the '
            'new toolchain.')
      cleanup = owns_work_dir and not args.keep
    else:
      print('\nDry run (no --upload). Built tarballs:')
      for suffix in tgzs:
        print('  %-8s %s' % (suffix, tgzs[suffix]))
        print('           sha256 %s' % digests[suffix])
      print(
          '\nRe-run with --upload to push to GCS and patch install-build-deps.')
  finally:
    if cleanup:
      shutil.rmtree(work_dir, ignore_errors=True)
    elif owns_work_dir:
      print('\nTarballs left in %s' % work_dir)


if __name__ == '__main__':
  main()
