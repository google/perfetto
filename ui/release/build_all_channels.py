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
""" Builds all the revisions in channels.json and deploys them if --upload.

See go/perfetto-ui-autopush for docs on how this works end-to-end.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

from os.path import dirname

pjoin = os.path.join

BUCKET_NAME = 'ui.perfetto.dev'
CUR_DIR = dirname(os.path.abspath(__file__))
ROOT_DIR = dirname(dirname(CUR_DIR))


def check_call_and_log(args):
  print(' '.join(args))
  subprocess.check_call(args)


def check_output(args):
  return subprocess.check_output(args).decode().strip()


def version_exists(version):
  url = 'https://commondatastorage.googleapis.com/%s/%s/manifest.json' % (
      BUCKET_NAME, version)
  return 0 == subprocess.call(['curl', '-fLs', '-o', '/dev/null', url])


def build_git_revision(channel, git_ref, tmp_dir):
  workdir = pjoin(tmp_dir, channel)
  check_call_and_log(['rm', '-rf', workdir])
  check_call_and_log(['git', 'clone', '--quiet', '--shared', ROOT_DIR, workdir])
  old_cwd = os.getcwd()
  os.chdir(workdir)
  try:
    check_call_and_log(['git', 'reset', '--hard', git_ref])
    check_call_and_log(['git', 'clean', '-dfx'])
    git_sha = check_output(['git', 'rev-parse', 'HEAD'])
    print('===================================================================')
    print('Building UI for channel %s @ %s (%s)' % (channel, git_ref, git_sha))
    print('===================================================================')
    version = check_output(['tools/write_version_header.py', '--stdout'])
    check_call_and_log(['tools/install-build-deps', '--ui'])
    check_call_and_log(['ui/build'])
    return version, pjoin(workdir, 'ui/out/dist')
  finally:
    os.chdir(old_cwd)


def build_all_channels(channels, tmp_dir, merged_dist_dir):
  channel_map = {}
  for chan in channels:
    channel = chan['name']
    git_ref = chan['rev']
    # version here is something like "v1.2.3".
    version, dist_dir = build_git_revision(channel, git_ref, tmp_dir)
    channel_map[channel] = version
    check_call_and_log(['cp', '-an', pjoin(dist_dir, version), merged_dist_dir])
    if channel != 'stable':
      continue
    # Copy also the /index.html and /service_worker.*, but only for the stable
    # channel. The /index.html and SW must be shared between all channels,
    # because they are all reachable through ui.perfetto.dev/. Both the index
    # and the SQ are supposed to be version-independent (go/perfetto-channels).
    # If an accidental incompatibility bug sneaks in, we should much rather
    # crash canary (or any other channel) rather than stable. Hence why we copy
    # the index+sw from the stable channel.
    for fname in os.listdir(dist_dir):
      fpath = pjoin(dist_dir, fname)
      if os.path.isfile(fpath):
        check_call_and_log(['cp', '-an', fpath, merged_dist_dir])
  return channel_map


def main():
  parser = argparse.ArgumentParser()
  parser.add_argument('--upload', action='store_true')
  parser.add_argument('--tmp', default='/tmp/perfetto_ui')
  parser.add_argument('--branch_only')

  args = parser.parse_args()

  # Read the releases.json, which maps channel names to git refs, e.g.:
  # {name:'stable', rev:'a0b1c2...0}, {name:'canary', rev:'HEAD'}
  channels = []
  with open(pjoin(CUR_DIR, 'channels.json')) as f:
    channels = json.load(f)['channels']

  if args.branch_only:
    channels = [{'name': 'branch', 'rev': args.branch_only}]

  merged_dist_dir = pjoin(args.tmp, 'dist')
  check_call_and_log(['rm', '-rf', merged_dist_dir])
  shutil.os.makedirs(merged_dist_dir)
  channel_map = build_all_channels(channels, args.tmp, merged_dist_dir)

  if not args.branch_only:
    print('Updating index in ' + merged_dist_dir)
    with open(pjoin(merged_dist_dir, 'index.html'), 'r+') as f:
      index_html = f.read()
      f.seek(0, 0)
      f.truncate()
      index_html = re.sub(
          r"data-perfetto_version='[^']*'",
          "data-perfetto_version='%s'" % json.dumps(channel_map), index_html)
      f.write(index_html)

  if not args.upload:
    return

  print('===================================================================')
  print('Uploading to gs://%s' % BUCKET_NAME)
  print('===================================================================')
  for name in os.listdir(merged_dist_dir):
    path = pjoin(merged_dist_dir, name)
    if os.path.isdir(path):
      if version_exists(name):
        print('Skipping upload of %s because it already exists on GCS' % name)
        continue
      # Do NOT set cache-headers here, it messes up with GCS' handling of
      # transparent gzip content-encoding. Public cache-control headers are set
      # by the GAE instance (see /infra/ui.perfetto.dev/appengine/main.py).
      # Do NOT add 'html' to the -z list. We want to set 'no-cache' on
      # index.html to prevent that the GAE instance is subject to caching on
      # autopush, but cache-control and -z are mutually exclusive (b/327213431).
      cp_cmd = [
          'gsutil', '-m', 'cp', '-z', 'js,json,css,wasm,map', '-r', path,
          'gs://%s/' % BUCKET_NAME
      ]
      check_call_and_log(cp_cmd)
    else:
      # /index.html or /service_worker.js{,.map}
      cp_cmd = [
          'gsutil', '-h', 'Cache-Control: no-cache,no-transform', 'cp', path,
          'gs://%s/%s' % (BUCKET_NAME, name)
      ]
      check_call_and_log(cp_cmd)


if __name__ == '__main__':
  sys.exit(main())
