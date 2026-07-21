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
"""Rolls the pinned pnpm toolchain.

Downloads the official prebuilt pnpm standalone binaries for every platform
Perfetto supports, and optionally uploads them to gs://perfetto and rewrites
the pins in tools/install-build-deps.

Usage:
  tools/roll_pnpm latest            # download binaries locally, print pins
  tools/roll_pnpm 9.15.4 --upload   # upload to GCS and patch deps
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

GCS_BUCKET = 'gs://perfetto'

# (suffix used in GCS object name and install-build-deps, candidate asset names regexes)
PLATFORMS = [
    ('linux-arm64', [r'pnpm-linux-arm64(\.tar\.gz)?$']),
    ('linux-x64', [r'pnpm-linux-x64(\.tar\.gz)?$']),
    ('macos-arm64', [r'pnpm-(macos|darwin)-arm64(\.tar\.gz)?$']),
    ('macos-x64', [r'pnpm-(macos|darwin)-x64(\.tar\.gz)?$']),
]


def sha256_file(path):
  h = hashlib.sha256()
  with open(path, 'rb') as f:
    for chunk in iter(lambda: f.read(1 << 20), b''):
      h.update(chunk)
  return h.hexdigest()


def fetch_release_info(tag):
  url = f'https://api.github.com/repos/pnpm/pnpm/releases/tags/{tag}'
  req = urllib.request.Request(
      url, headers={'User-Agent': 'Perfetto-Roll-Script'})
  with urllib.request.urlopen(req) as f:
    return json.load(f)


def download_binary(suffix, patterns, release_assets, version, work_dir):
  """Downloads and extracts one platform binary. Returns (file path, sha256)."""
  asset_url = None
  asset_name = None

  for asset in release_assets:
    name = asset['name']
    for pat in patterns:
      if re.match(pat, name):
        asset_url = asset['browser_download_url']
        asset_name = name
        break
    if asset_url:
      break

  if not asset_url:
    raise SystemExit(
        f'Could not find matching asset for platform {suffix} in release v{version}'
    )

  download_file = os.path.join(work_dir, asset_name)
  binary_path = os.path.join(work_dir, f'pnpm-{suffix}-{version}')

  print(f'[{suffix}] downloading {asset_url}')
  subprocess.check_call(
      ['curl', '-f', '-L', '-#', '-o', download_file, asset_url])

  if asset_name.endsWith('.tar.gz') if hasattr(
      asset_name, 'endsWith') else asset_name.endswith('.tar.gz'):
    print(f'[{suffix}] extracting tar.gz')
    extract_dir = os.path.join(work_dir, f'extract-{suffix}')
    os.makedirs(extract_dir, exist_ok=True)
    subprocess.check_call(['tar', '-xf', download_file, '-C', extract_dir])
    # The binary inside the tar archive is usually named `pnpm` or `pnpm-xxx`
    extracted_files = [
        os.path.join(extract_dir, f)
        for f in os.listdir(extract_dir)
        if not os.path.isdir(os.path.join(extract_dir, f))
    ]
    if not extracted_files:
      # Check subdirectories if present
      for root, _, files in os.walk(extract_dir):
        for f in files:
          extracted_files.append(os.path.join(root, f))
    if not extracted_files:
      raise SystemExit(f'No binary found in tarball for {suffix}')
    shutil.copy(extracted_files[0], binary_path)
    shutil.rmtree(extract_dir)
    os.remove(download_file)
  else:
    shutil.move(download_file, binary_path)

  os.chmod(binary_path, 0o755)
  digest = sha256_file(binary_path)
  print(f'[{suffix}] sha256 {digest}')
  return binary_path, digest


def upload(binary_path, version, suffix):
  target = f'{GCS_BUCKET}/pnpm-{suffix}-{version}'
  print(f'uploading {os.path.basename(binary_path)} -> {target}')
  subprocess.check_call([
      'gcloud', 'storage', 'cp', '--no-clobber', '--predefined-acl=publicRead',
      binary_path, target
  ])


def patch_install_build_deps(version, digests):
  with open(INSTALL_BUILD_DEPS) as f:
    text = f.read()

  patterns = {
      'linux-arm64':
          re.compile(
              r"(Dependency\(\s*'third_party/pnpm/pnpm',\s*')[^']+(\s*',\s*')[0-9a-f]{64}(\s*',\s*'linux',\s*'arm64'\))"
          ),
      'linux-x64':
          re.compile(
              r"(Dependency\(\s*'third_party/pnpm/pnpm',\s*')[^']+(\s*',\s*')[0-9a-f]{64}(\s*',\s*'linux',\s*'x64'\))"
          ),
      'macos-arm64':
          re.compile(
              r"(Dependency\(\s*'third_party/pnpm/pnpm',\s*')[^']+(\s*',\s*')[0-9a-f]{64}(\s*',\s*'darwin',\s*'arm64'\))"
          ),
      'macos-x64':
          re.compile(
              r"(Dependency\(\s*'third_party/pnpm/pnpm',\s*')[^']+(\s*',\s*')[0-9a-f]{64}(\s*',\s*'darwin',\s*'x64'\))"
          ),
  }

  for suffix, pat in patterns.items():
    digest = digests[suffix]
    replacement = r"\g<1>https://storage.googleapis.com/perfetto/pnpm-%s-%s\g<2>%s\g<3>" % (
        suffix, version, digest)
    text, n = pat.subn(replacement, text)
    if n != 1:
      raise SystemExit(
          'Expected exactly one %s pin in install-build-deps, found %d' %
          (suffix, n))

  with open(INSTALL_BUILD_DEPS, 'w') as f:
    f.write(text)
  print(f'patched {INSTALL_BUILD_DEPS}')


PNPM_RELEASES_URL = 'https://api.github.com/repos/pnpm/pnpm/releases'


def check_has_all_assets(release):
  assets = [a['name'] for a in release.get('assets', [])]
  for _, patterns in PLATFORMS:
    matched = False
    for pat in patterns:
      if any(re.match(pat, name) for name in assets):
        matched = True
        break
    if not matched:
      return False
  return True


def resolve_pnpm_version(query):
  query = query.lstrip('v').strip()

  if query == 'latest':
    req = urllib.request.Request(
        PNPM_RELEASES_URL, headers={'User-Agent': 'Perfetto-Roll-Script'})
    with urllib.request.urlopen(req) as f:
      releases = json.load(f)
    for r in releases:
      if not r.get('prerelease', False) and check_has_all_assets(r):
        return r['tag_name'].lstrip('v')
    return releases[0]['tag_name'].lstrip('v')

  return query


def main():
  parser = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument(
      'version', help='pnpm release version, e.g. 9.15.4 or latest')
  parser.add_argument(
      '--upload',
      action='store_true',
      help='upload binaries to GCS and patch install-build-deps')
  parser.add_argument(
      '--work-dir',
      help='scratch dir for downloads (default: temp dir under repo root)')
  parser.add_argument(
      '--platforms',
      help='comma-separated subset of: ' + ', '.join(p[0] for p in PLATFORMS))
  parser.add_argument(
      '--keep', action='store_true', help='keep the work dir when done')
  args = parser.parse_args()

  version = resolve_pnpm_version(args.version)
  print(f'Resolved pnpm version: {version}')

  release_info = fetch_release_info(f'v{version}')
  assets = release_info.get('assets', [])

  wanted = set(args.platforms.split(',')) if args.platforms else None
  platforms = [p for p in PLATFORMS if wanted is None or p[0] in wanted]

  owns_work_dir = not args.work_dir
  work_dir = args.work_dir or tempfile.mkdtemp(
      prefix='pnpm-roll-', dir=ROOT_DIR)
  os.makedirs(work_dir, exist_ok=True)
  print(f'work dir: {work_dir}')

  cleanup = False
  try:
    digests, binaries = {}, {}
    for suffix, patterns in platforms:
      bin_path, digest = download_binary(suffix, patterns, assets, version,
                                         work_dir)
      digests[suffix] = digest
      binaries[suffix] = bin_path

    if args.upload:
      for suffix in binaries:
        upload(binaries[suffix], version, suffix)
      patch_install_build_deps(version, digests)
      print('\nDone. Run `tools/install-build-deps` to fetch and verify the '
            'new pnpm toolchain.')
      cleanup = owns_work_dir and not args.keep
    else:
      print('\nDry run (no --upload). Downloaded binaries:')
      for suffix in binaries:
        print('  %-12s %s' % (suffix, binaries[suffix]))
        print('               sha256 %s' % digests[suffix])
      print(
          '\nRe-run with --upload to push to GCS and patch install-build-deps.')
  finally:
    if cleanup:
      shutil.rmtree(work_dir, ignore_errors=True)
    elif owns_work_dir:
      print(f'\nBinaries left in {work_dir}')


if __name__ == '__main__':
  main()
