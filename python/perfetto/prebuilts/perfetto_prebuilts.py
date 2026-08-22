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
"""
Functions to fetch pre-pinned Perfetto prebuilts.

This function is used in different places:
- Into the //tools/{trace_processor, traceconv} scripts, which are just plain
  wrappers around executables.
- Into the //tools/{heap_profiler, record_android_trace} scripts, which contain
  some other hand-written python code.

The manifest argument looks as follows:
TRACECONV_MANIFEST = [
  {
    'arch': 'mac-amd64',
    'file_name': 'traceconv',
    'file_size': 7087080,
    'url': https://commondatastorage.googleapis.com/.../trace_to_text',
    'sha256': 7d957c005b0dc130f5bd855d6cec27e060d38841b320d04840afc569f9087490',
    'platform': 'darwin',
    'machine': 'x86_64'
  },
  ...
]

The intended usage is:

  from perfetto.prebuilts.manifests.traceconv import TRACECONV_MANIFEST
  bin_path = get_perfetto_prebuilt(TRACECONV_MANIFEST)
  subprocess.call(bin_path, ...)
"""

import hashlib
import os
import platform
import random
import subprocess
import sys


def download_or_get_cached(file_name, url, sha256):
  """ Downloads a prebuilt or returns a cached version

  The first time this is invoked, it downloads the |url| and caches it into
  ~/.local/share/perfetto/prebuilts/$tool_name-$sha256. On subsequent
  invocations it just runs the cached version.

  The (short) SHA-256 is embedded in the cached file name so that several
  versions of the same tool can coexist on the same machine. This matters when
  e.g. two virtualenvs pin different Perfetto releases, or the //tools wrappers
  and the Python API request different versions: without the SHA in the name
  they would all map to the same path, clobber each other and trigger a
  re-download on every switch.
  """
  dir = os.path.join(
      os.path.expanduser('~'), '.local', 'share', 'perfetto', 'prebuilts')
  os.makedirs(dir, exist_ok=True)
  # Embed the SHA in the file name, preserving any extension (e.g. .exe) as the
  # last component since callers (and the OS, on Windows) rely on it.
  root, ext = os.path.splitext(file_name)
  bin_path = os.path.join(dir, '%s-%s%s' % (root, sha256[:16], ext))

  # The cached file is only ever created via an atomic rename after the SHA-256
  # has been verified, so if a file at this (SHA-named) path exists we can trust
  # it without recomputing the hash on every invocation.
  if os.path.exists(bin_path):
    return bin_path

  # Use a unique random file to guard against concurrent executions.
  # See https://github.com/google/perfetto/issues/786 .
  tmp_path = '%s.%d.tmp' % (bin_path, random.randint(0, 100000))
  print('Downloading ' + url)
  subprocess.check_call(['curl', '-f', '-L', '-#', '-o', tmp_path, url])
  with open(tmp_path, 'rb') as fd:
    actual_sha256 = hashlib.sha256(fd.read()).hexdigest()
  if actual_sha256 != sha256:
    raise Exception('Checksum mismatch for %s (actual: %s, expected: %s)' %
                    (url, actual_sha256, sha256))
  os.chmod(tmp_path, 0o755)
  os.replace(tmp_path, bin_path)
  return bin_path


def get_perfetto_prebuilt(manifest, soft_fail=False, arch=None):
  """ Downloads the prebuilt, if necessary, and returns its path on disk. """
  plat = sys.platform.lower()
  machine = platform.machine().lower()
  manifest_entry = None
  for entry in manifest:
    # If the caller overrides the arch, just match that (for Android prebuilts).
    if arch:
      if entry.get('arch') == arch:
        manifest_entry = entry
        break
      continue
    # Otherwise guess the local machine arch.
    if entry.get('platform') == plat and machine in entry.get('machine', []):
      manifest_entry = entry
      break
  if manifest_entry is None:
    if soft_fail:
      return None
    raise Exception(
        ('No prebuilts available for %s-%s\n' % (plat, machine)) +
        'See https://perfetto.dev/docs/contributing/build-instructions')

  # Placeholder entries (e.g. before a release has been rolled) have an empty
  # URL. Treat them the same as a missing entry when soft_fail is set.
  if not manifest_entry.get('url'):
    if soft_fail:
      return None
    raise Exception('No prebuilt URL available for %s on %s-%s. '
                    'The prebuilt may not have been rolled yet.' %
                    (manifest_entry.get('file_name', '?'), plat, machine))

  return download_or_get_cached(
      file_name=manifest_entry['file_name'],
      url=manifest_entry['url'],
      sha256=manifest_entry['sha256'])


def run_perfetto_prebuilt(manifest):
  bin_path = get_perfetto_prebuilt(manifest)
  if sys.platform.lower() == 'win32':
    sys.exit(subprocess.check_call([bin_path, *sys.argv[1:]]))
  os.execv(bin_path, [bin_path] + sys.argv[1:])
