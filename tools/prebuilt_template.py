#!/usr/bin/env python3
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

# This is the template used by tools/roll-prebuilts to generate the script
# tools/{trace_processor, traceconv, tracebox} which in turn are served by
# https://get.perfetto.dev/{trace_processor, traceconv, tracebox}
#
# This file should do the same thing when being invoked in any of these ways:
# ./tool_name
# python tool_name
# bash tool_name
# cat ./tool_name | bash
# cat ./tool_name | python -

BASH_FALLBACK = """ "
exec python3 - "$@" <<'#'EOF
#"""

import hashlib
import os
import platform
import subprocess
import sys

# The placeholder below will be replaced with something like:
# TOOL_NAME = 'trace_processor_shell'
# MANIFEST = [{'tool': 'trace_processor_shell', ...]
TOOL_NAME = ''
MANIFEST = []

# REPLACEMENT_PLACEHOLDER


# The first time this is invoked, it downloads the |url| and caches it into
# ~/.perfetto/prebuilts/$tool_name. On subsequent invocations it just runs the
# cached version.
def download_or_get_cached(file_name, url, sha256):
  dir = os.path.join(os.path.expanduser('~'), '.local', 'share', 'perfetto', 'prebuilts')
  os.makedirs(dir, exist_ok=True)
  bin_path = os.path.join(dir, file_name)
  sha256_path = os.path.join(dir, file_name + '.sha256')
  needs_download = True

  # Avoid recomputing the SHA-256 on each invocation. The SHA-256 of the last
  # download is cached into file_name.sha256, just check if that matches.
  if os.path.exists(bin_path) and os.path.exists(sha256_path):
    with open(sha256_path, 'rb') as f:
      digest = f.read().decode()
      if digest == sha256:
        needs_download = False

  if needs_download:
    # Either the filed doesn't exist or the SHA256 doesn't match.
    tmp_path = bin_path + '.tmp'
    print('Downloading ' + url)
    subprocess.check_call(['curl', '-f', '-L', '-#', '-o', tmp_path, url])
    with open(tmp_path, 'rb') as fd:
      actual_sha256 = hashlib.sha256(fd.read()).hexdigest()
    if actual_sha256 != sha256:
      raise 'Checksum mismatch for %s (actual: %s, expected: %s)' % (
          url, actual_sha256, sha256)
    os.chmod(tmp_path, 0o755)
    os.rename(tmp_path, bin_path)
    with open(sha256_path, 'w') as f:
      f.write(sha256)
  return bin_path


def main(argv):
  plat = sys.platform.lower()
  machine = platform.machine().lower()
  manifest = None
  for entry in MANIFEST:
    if entry.get('tool') == TOOL_NAME and entry.get(
        'platform') == plat and entry.get('machine') == machine:
      manifest = entry
      break
  if manifest is None:
    print('No prebuilts available for %s/%s' % (plat, machine))
    print('See https://perfetto.dev/docs/contributing/build-instructions')
    return 1
  bin_path = download_or_get_cached(
      file_name=manifest['file_name'],
      url=manifest['url'],
      sha256=manifest['sha256'])
  os.execv(bin_path, [bin_path] + argv[1:])


if __name__ == '__main__':
  sys.exit(main(sys.argv))

#EOF
