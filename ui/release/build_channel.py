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
"""Builds a single UI channel and deploys it to GCS.

Designed to run inside the perfetto-ui-builder Docker image, invoked from
infra/ui.perfetto.dev/cloudbuild*.yaml. Cloud Build has placed us on the
SHA that fired the trigger; this script builds the UI from HEAD and uploads
to gs://ui.perfetto.dev/v<version>/.

The shared root /index.html (and /bigtrace.html, if produced) carries a
data-perfetto_version='{"stable":...,"canary":...,"autopush":...}' map.
Each channel's deploy modifies ONLY its own entry in that map, using GCS
x-goog-if-generation-match as a compare-and-swap primitive so parallel
deploys cannot lose updates. The stable channel additionally swaps the
HTML body itself (still under CAS, still preserving the other two
channels' map entries) and uploads the shared /service_worker.* files.
Canary and autopush never write the HTML body or service_worker --
preserves the invariant that canary instability cannot break stable users.

See go/perfetto-ui-autopush for end-to-end docs.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time

from os.path import dirname

pjoin = os.path.join

BUCKET_NAME = 'ui.perfetto.dev'
CHANNELS = ('autopush', 'canary', 'stable')
CUR_DIR = dirname(os.path.abspath(__file__))
ROOT_DIR = dirname(dirname(CUR_DIR))
CAS_MAX_RETRIES = 10
CACHE_NO_CACHE = 'Cache-Control: no-cache, no-transform'

VERSION_ATTR_RE = re.compile(r"data-perfetto_version='([^']*)'")
GENERATION_RE = re.compile(r'^\s*Generation:\s+(\d+)', re.MULTILINE)


def check_call_and_log(args):
  print(' '.join(args))
  subprocess.check_call(args)


def check_output_str(args):
  return subprocess.check_output(args).decode().strip()


def gsutil_stat_generation(gcs_path):
  """Returns the GCS object's generation number, or None if it does not
  exist."""
  result = subprocess.run(
      ['gsutil', 'stat',
       'gs://%s/%s' % (BUCKET_NAME, gcs_path)],
      capture_output=True)
  if result.returncode != 0:
    return None
  m = GENERATION_RE.search(result.stdout.decode())
  if not m:
    raise Exception('Could not parse Generation from gsutil stat output:\n' +
                    result.stdout.decode())
  return m.group(1)


def gsutil_cat_at_generation(gcs_path, generation):
  """Returns the body of the object at the specific generation, or None
  if that generation is no longer the live one (i.e. somebody else wrote
  between our stat and our cat). Pinning the read to a specific
  generation makes stat+cat atomic, so the body we return matches the
  generation we will compare-and-swap against at write time."""
  result = subprocess.run(
      ['gsutil', 'cat',
       'gs://%s/%s#%s' % (BUCKET_NAME, gcs_path, generation)],
      capture_output=True)
  if result.returncode != 0:
    return None
  # Note: do NOT strip; we need to preserve the file content exactly.
  return result.stdout.decode()


def version_exists(version):
  url = 'https://commondatastorage.googleapis.com/%s/%s/manifest.json' % (
      BUCKET_NAME, version)
  return 0 == subprocess.call(['curl', '-fLs', '-o', '/dev/null', url])


def build(channel):
  os.chdir(ROOT_DIR)
  git_sha = check_output_str(['git', 'rev-parse', 'HEAD'])
  print('===================================================================')
  print('Building UI for channel %s @ %s' % (channel, git_sha))
  print('===================================================================')
  version = check_output_str(['tools/write_version_header.py', '--stdout'])
  check_call_and_log(['tools/install-build-deps', '--ui'])
  check_call_and_log(['ui/build'])
  return version, pjoin(ROOT_DIR, 'ui/out/dist')


def upload_versioned_dir(version, dist_dir):
  versioned_path = pjoin(dist_dir, version)
  if version_exists(version):
    print('Skipping upload of %s because it already exists on GCS' % version)
    return
  # Do NOT set cache-control here; it conflicts with GCS' transparent
  # gzip handling (b/327213431). Public cache-control is set by AppEngine
  # (infra/ui.perfetto.dev/appengine/main.py).
  check_call_and_log([
      'gsutil', '-m', 'cp', '-z', 'js,json,css,wasm,map', '-r', versioned_path,
      'gs://%s/' % BUCKET_NAME
  ])


def cas_write_html(gcs_path, update_fn):
  """Read gs://BUCKET/<gcs_path>, apply update_fn(text)->text, write back
  under x-goog-if-generation-match. Retry on precondition failure.

  If the object doesn't exist, update_fn is called with '' and the write
  uses if-generation-match:0 (must-not-exist semantics).
  """
  for attempt in range(CAS_MAX_RETRIES):
    gen = gsutil_stat_generation(gcs_path)
    if gen is None:
      print('%s does not exist yet on GCS; will write with '
            'if-generation-match:0' % gcs_path)
      gen = '0'
      current = ''
    else:
      current = gsutil_cat_at_generation(gcs_path, gen)
      if current is None:
        print('cat of %s#%s failed (object likely bumped between stat and '
              'cat); retrying' % (gcs_path, gen))
        time.sleep(1 + attempt)
        continue

    new = update_fn(current)
    if new == current:
      print('No change needed for %s; skipping write' % gcs_path)
      return

    tmp_fd, tmp_path = tempfile.mkstemp(suffix='.html')
    os.close(tmp_fd)
    try:
      with open(tmp_path, 'w') as f:
        f.write(new)
      ret = subprocess.call([
          'gsutil',
          '-h',
          'x-goog-if-generation-match:%s' % gen,
          '-h',
          CACHE_NO_CACHE,
          'cp',
          tmp_path,
          'gs://%s/%s' % (BUCKET_NAME, gcs_path),
      ])
    finally:
      os.unlink(tmp_path)

    if ret == 0:
      print('CAS write of %s succeeded on attempt %d' % (gcs_path, attempt + 1))
      return
    print('CAS write of %s failed (likely 412 precondition); retrying' %
          gcs_path)
    time.sleep(1 + attempt)

  raise Exception('CAS retries exhausted for ' + gcs_path)


def parse_version_map(html):
  m = VERSION_ATTR_RE.search(html)
  if not m:
    return None
  return json.loads(m.group(1))


def replace_version_map(html, version_map):
  return VERSION_ATTR_RE.sub(
      "data-perfetto_version='%s'" % json.dumps(version_map), html, count=1)


def patch_one_channel(html, channel, new_version):
  """Replace ONLY <channel>'s entry in the data-perfetto_version map.
  Preserve all other entries verbatim."""
  version_map = parse_version_map(html)
  if version_map is None:
    raise Exception('data-perfetto_version attribute not found in remote HTML; '
                    'refusing to write. The stable channel must seed the root '
                    'HTML before canary/autopush can update their entries.')
  if version_map.get(channel) == new_version:
    return html
  version_map[channel] = new_version
  return replace_version_map(html, version_map)


def make_stable_updater(local_html_path, new_version):
  """For stable: swap the HTML body to the freshly-built one, but splice
  in the existing canary/autopush entries from the current remote state."""
  with open(local_html_path) as f:
    new_body = f.read()

  def update(remote_html):
    existing = parse_version_map(remote_html) or {}
    merged = dict(existing)
    merged['stable'] = new_version
    out = replace_version_map(new_body, merged)
    if VERSION_ATTR_RE.search(new_body) is None:
      raise Exception('Locally-built HTML has no data-perfetto_version '
                      'attribute; ui/build did not bake it in correctly.')
    return out

  return update


def make_other_channel_updater(channel, new_version):
  """For canary/autopush: update only this channel's entry in the remote
  HTML's map, leaving the body untouched."""

  def update(remote_html):
    return patch_one_channel(remote_html, channel, new_version)

  return update


def upload_loose_files(channel, version, dist_dir):
  """Iterate the loose files in dist_dir (HTML files + service_worker.*).
  HTML files get CAS-updated by every channel. Non-HTML files are uploaded
  plain by the stable channel only and ignored by other channels."""
  for fname in sorted(os.listdir(dist_dir)):
    fpath = pjoin(dist_dir, fname)
    if not os.path.isfile(fpath):
      continue  # skip the v<version>/ subdir
    if fname.endswith('.html'):
      if channel == 'stable':
        update = make_stable_updater(fpath, version)
      else:
        update = make_other_channel_updater(channel, version)
      cas_write_html(fname, update)
    elif channel == 'stable':
      check_call_and_log([
          'gsutil',
          '-h',
          CACHE_NO_CACHE,
          'cp',
          fpath,
          'gs://%s/%s' % (BUCKET_NAME, fname),
      ])
    # else: canary/autopush ignore non-HTML loose files (e.g.
    # service_worker.*) -- only stable owns those.


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--channel', required=True, choices=CHANNELS)
  parser.add_argument('--upload', action='store_true')
  args = parser.parse_args()

  version, dist_dir = build(args.channel)

  if not args.upload:
    return

  print('===================================================================')
  print('Uploading channel %s @ %s to gs://%s' %
        (args.channel, version, BUCKET_NAME))
  print('===================================================================')
  upload_versioned_dir(version, dist_dir)
  upload_loose_files(args.channel, version, dist_dir)


if __name__ == '__main__':
  sys.exit(main())
