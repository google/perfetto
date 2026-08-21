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
"""Rolls the pinned syntaqlite version throughout the tree.

syntaqlite ships in two forms and both are pinned in-tree:

  * the CLI prebuilt binaries, hosted on GitHub releases and pinned (URL +
    sha256, per platform) in tools/install-build-deps. The CLI is used by
    tools/format-sql-sources and tools/gen_syntaqlite_parser.
  * the npm package, consumed by the UI and pinned in ui/package.json and
    ui/pnpm-lock.yaml.

This tool bumps every pin in lockstep. For the CLI it downloads each
platform release asset and recomputes its sha256; for the npm package it
reads the integrity hash straight from the registry. Nothing is uploaded:
the releases already exist upstream, we only re-point the tree at a new tag.

With --regen it also fetches the new CLI (tools/install-build-deps
--filter syntaqlite) and regenerates the vendored PerfettoSQL parser
(tools/gen_syntaqlite_parser), which the grammar codegen may change across
versions.

Usage:
  tools/roll_syntaqlite 0.7.1            # patch every pin in the tree
  tools/roll_syntaqlite 0.7.1 --regen    # also refetch CLI + regen parser
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.request

ROOT_DIR = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INSTALL_BUILD_DEPS = os.path.join(ROOT_DIR, 'tools', 'install-build-deps')
PACKAGE_JSON = os.path.join(ROOT_DIR, 'ui', 'package.json')
PNPM_LOCK = os.path.join(ROOT_DIR, 'ui', 'pnpm-lock.yaml')

# Per-platform release assets, as they appear in the install-build-deps URLs.
GITHUB_URL = ('https://github.com/LalitMaganti/syntaqlite/releases/download/'
              'v%s/syntaqlite-%s')
ARTIFACTS = [
    'linux-x64.tar.gz',
    'linux-arm64.tar.gz',
    'macos-x64.tar.gz',
    'macos-arm64.tar.gz',
    'windows-x64.zip',
    'windows-arm64.zip',
]

NPM_META_URL = 'https://registry.npmjs.org/syntaqlite'


def sha256_url(url):
  """Streams a URL and returns its sha256, without buffering the whole asset."""
  h = hashlib.sha256()
  with urllib.request.urlopen(url) as f:
    for chunk in iter(lambda: f.read(1 << 20), b''):
      h.update(chunk)
  return h.hexdigest()


def fetch_npm_integrity(version):
  with urllib.request.urlopen(NPM_META_URL) as f:
    meta = json.load(f)
  versions = meta.get('versions', {})
  if version not in versions:
    recent = ', '.join(sorted(versions, reverse=True)[:8])
    raise SystemExit('Unknown npm syntaqlite version %r. Recent: %s' %
                     (version, recent))
  return versions[version]['dist']['integrity']


def patch(path, subs):
  """Applies (pattern, repl, label) substitutions, requiring exactly one each."""
  with open(path) as f:
    text = f.read()
  for pat, repl, label in subs:
    text, n = re.subn(pat, repl, text)
    if n != 1:
      raise SystemExit('Expected exactly one %s in %s, found %d' %
                       (label, os.path.basename(path), n))
  with open(path, 'w') as f:
    f.write(text)
  print('patched %s' % os.path.relpath(path, ROOT_DIR))


def patch_install_build_deps(version, digests):
  subs = []
  for artifact, digest in digests.items():
    # Match the URL's version and the sha256 on the following line together so
    # both move in lockstep, anchored on the per-platform asset name.
    pat = (r"(/download/v)[0-9][0-9.]*(/syntaqlite-%s',\n\s*')[0-9a-f]{64}" %
           re.escape(artifact))
    subs.append((pat, r'\g<1>%s\g<2>%s' % (version, digest), artifact + ' pin'))
  patch(INSTALL_BUILD_DEPS, subs)


def patch_package_json(version):
  patch(PACKAGE_JSON, [(r'("syntaqlite":\s*"\^)[0-9][0-9.]*(")',
                        r'\g<1>%s\g<2>' % version, 'package.json version')])


def patch_pnpm_lock(version, integrity):
  subs = [
      (r'(syntaqlite:\n\s*specifier: \^)[0-9][0-9.]*(\n\s*version: )[0-9][0-9.]*',
       r'\g<1>%s\g<2>%s' % (version, version), 'importer entry'),
      (r'(/syntaqlite@)[0-9][0-9.]*(:\n\s*resolution: \{integrity: )'
       r'sha512-[A-Za-z0-9+/=]+', r'\g<1>%s\g<2>%s' % (version, integrity),
       'resolution entry'),
  ]
  patch(PNPM_LOCK, subs)


def regen():
  print('\nfetching new CLI: tools/install-build-deps --filter syntaqlite')
  subprocess.check_call([
      os.path.join(ROOT_DIR, 'tools', 'install-build-deps'), '--filter',
      'syntaqlite'
  ])
  print('\nregenerating parser: tools/gen_syntaqlite_parser')
  subprocess.check_call(
      [os.path.join(ROOT_DIR, 'tools', 'gen_syntaqlite_parser')])


def main():
  parser = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument('version', help='syntaqlite release, e.g. 0.7.1')
  parser.add_argument(
      '--regen',
      action='store_true',
      help='also fetch the new CLI and regenerate the vendored parser')
  args = parser.parse_args()
  version = args.version.lstrip('v')

  print('rolling syntaqlite -> %s' % version)

  digests = {}
  for artifact in ARTIFACTS:
    url = GITHUB_URL % (version, artifact)
    print('[%s] hashing %s' % (artifact, url))
    digests[artifact] = sha256_url(url)
    print('[%s] sha256 %s' % (artifact, digests[artifact]))

  integrity = fetch_npm_integrity(version)
  print('npm integrity %s' % integrity)

  patch_install_build_deps(version, digests)
  patch_package_json(version)
  patch_pnpm_lock(version, integrity)

  if args.regen:
    regen()
    print('\nDone. Review the regenerated parser and lockfile, then build.')
  else:
    print('\nDone patching pins. Re-run with --regen to refetch the CLI and '
          'regenerate the vendored parser, or run those steps by hand:\n'
          '  tools/install-build-deps --syntaqlite\n'
          '  tools/gen_syntaqlite_parser')


if __name__ == '__main__':
  main()
