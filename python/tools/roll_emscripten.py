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
"""Rolls the pinned Emscripten (WASM) toolchain.

Resolves an emscripten release (e.g. 5.0.3) to its emscripten-releases build
hash, downloads the prebuilt LLVM/Binaryen/Emscripten bundle for every platform
Perfetto supports, repackages each into the flat `emsdk/` layout expected by
gn/standalone/.emscripten, and optionally uploads them to gs://perfetto and
rewrites the pins in tools/install-build-deps.

No local emsdk checkout or `emsdk install/activate` is needed. The version to
build-hash mapping comes from the upstream emscripten-releases-tags.json, and
every platform bundle (including both macOS variants) is fetched from the
upstream GCS build bucket. A single Linux host can therefore produce all three
tarballs.

Usage:
  tools/roll_emscripten 5.0.3            # build tarballs locally, print pins
  tools/roll_emscripten 5.0.3 --upload   # also upload to GCS and patch deps
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

# Maps an emscripten release (and aliases like "latest") to the git hash of the
# emscripten-releases build that ships that version.
TAGS_URL = ('https://raw.githubusercontent.com/emscripten-core/emsdk/main/'
            'emscripten-releases-tags.json')

# Upstream prebuilt bundles, keyed by (os dir, build hash, arch suffix). The
# archive unpacks to a top-level install/ directory.
BUILD_URL = (
    'https://storage.googleapis.com/webassembly/'
    'emscripten-releases-builds/{os}/{hash}/wasm-binaries{arch}.tar.xz')

GCS_BUCKET = 'gs://perfetto'

# (pin suffix used in the GCS object name and install-build-deps, upstream os
# dir, upstream arch suffix baked into the archive name). The pin suffix matches
# the existing emscripten-<version>-<suffix>.tgz naming.
PLATFORMS = [
    ('linux', 'linux', ''),
    ('mac', 'mac', '-arm64'),
    ('mac-x64', 'mac', ''),
]


def resolve_hash(version):
  with urllib.request.urlopen(TAGS_URL) as f:
    tags = json.load(f)
  version = tags.get('aliases', {}).get(version, version)
  releases = tags['releases']
  if version not in releases:
    recent = ', '.join(sorted(releases, reverse=True)[:8])
    raise SystemExit('Unknown emscripten version %r. Recent releases: %s' %
                     (version, recent))
  return version, releases[version]


def sha256_file(path):
  h = hashlib.sha256()
  with open(path, 'rb') as f:
    for chunk in iter(lambda: f.read(1 << 20), b''):
      h.update(chunk)
  return h.hexdigest()


def build_tarball(suffix, os_dir, arch, release_hash, version, work_dir):
  """Downloads and repackages one platform bundle. Returns (tgz path, sha256)."""
  url = BUILD_URL.format(os=os_dir, hash=release_hash, arch=arch)
  archive = os.path.join(work_dir, 'upstream-%s.tar.xz' % suffix)
  extract = os.path.join(work_dir, 'extract-%s' % suffix)
  tgz = os.path.join(work_dir, 'emscripten-%s-%s.tgz' % (version, suffix))

  print('[%s] downloading %s' % (suffix, url))
  subprocess.check_call(['curl', '-f', '-L', '-#', '-o', archive, url])

  print('[%s] extracting' % suffix)
  if os.path.isdir(extract):
    shutil.rmtree(extract)
  os.makedirs(extract)
  subprocess.check_call(['tar', '-xf', archive, '-C', extract])

  # The upstream archive unpacks to install/; gn/standalone/.emscripten expects
  # the toolchain rooted at buildtools/<os>/emsdk/.
  os.rename(os.path.join(extract, 'install'), os.path.join(extract, 'emsdk'))

  print('[%s] packaging %s' % (suffix, os.path.basename(tgz)))
  subprocess.check_call(['tar', '-C', extract, '-czf', tgz, 'emsdk'])

  # Drop the intermediates now to keep peak disk usage to one bundle at a time.
  os.remove(archive)
  shutil.rmtree(extract)

  digest = sha256_file(tgz)
  print('[%s] sha256 %s' % (suffix, digest))
  return tgz, digest


def upload(tgz, version, suffix):
  target = '%s/emscripten-%s-%s.tgz' % (GCS_BUCKET, version, suffix)
  print('uploading %s -> %s' % (os.path.basename(tgz), target))
  subprocess.check_call(
      ['gsutil', 'cp', '-n', '-a', 'public-read', tgz, target])


def patch_install_build_deps(version, digests):
  with open(INSTALL_BUILD_DEPS) as f:
    text = f.read()
  for suffix, digest in digests.items():
    # Match the URL's version and the sha256 on the following line together, so
    # both move in lockstep. The -mac.tgz pattern does not match -mac-x64.tgz.
    pat = re.compile(
        r"(emscripten-)[0-9][0-9.]*(-%s\.tgz',\n\s*')[0-9a-f]{64}" %
        re.escape(suffix))
    text, n = pat.subn(r"\g<1>%s\g<2>%s" % (version, digest), text)
    if n != 1:
      raise SystemExit(
          'Expected exactly one %s pin in install-build-deps, found %d' %
          (suffix, n))
  with open(INSTALL_BUILD_DEPS, 'w') as f:
    f.write(text)
  print('patched %s' % INSTALL_BUILD_DEPS)


def main():
  parser = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument(
      'version', help='emscripten release, e.g. 5.0.3 or latest')
  parser.add_argument(
      '--upload',
      action='store_true',
      help='upload tarballs to GCS and patch install-build-deps')
  parser.add_argument(
      '--work-dir',
      help='scratch dir for downloads/repackaging (default: a temp dir under '
      'the repo root; needs a few GB free and a real disk, not tmpfs)')
  parser.add_argument(
      '--platforms',
      help='comma-separated subset of: ' + ', '.join(p[0] for p in PLATFORMS))
  parser.add_argument(
      '--keep', action='store_true', help='keep the work dir when done')
  args = parser.parse_args()

  version, release_hash = resolve_hash(args.version)
  print('emscripten %s -> %s' % (version, release_hash))

  wanted = set(args.platforms.split(',')) if args.platforms else None
  platforms = [p for p in PLATFORMS if wanted is None or p[0] in wanted]

  owns_work_dir = not args.work_dir
  work_dir = args.work_dir or tempfile.mkdtemp(
      prefix='emscripten-roll-', dir=ROOT_DIR)
  os.makedirs(work_dir, exist_ok=True)
  print('work dir: %s' % work_dir)

  cleanup = False
  try:
    digests, tgzs = {}, {}
    for suffix, os_dir, arch in platforms:
      tgz, digest = build_tarball(suffix, os_dir, arch, release_hash, version,
                                  work_dir)
      digests[suffix] = digest
      tgzs[suffix] = tgz

    if args.upload:
      for suffix in tgzs:
        upload(tgzs[suffix], version, suffix)
      patch_install_build_deps(version, digests)
      print('\nDone. Run `tools/install-build-deps` to fetch and verify the '
            'new toolchain, then build the wasm target.')
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
