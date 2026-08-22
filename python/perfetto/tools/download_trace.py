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
"""Download a Perfetto trace from a ui.perfetto.dev share link.

A share/permalink looks like `https://ui.perfetto.dev/#!/?s=<40-char-hex-hash>`.
The hash names a JSON "permalink" object in GCS which embeds a `traceUrl`
pointing at the actual trace file. This module resolves that chain and
downloads the trace.
"""

import argparse
import json
import os
import re
import ssl
import sys
import urllib.error
import urllib.parse
import urllib.request

BUCKET_NAME = 'perfetto-ui-data'
HASH_RE = re.compile(r'^[a-fA-F0-9]{40}$')


def extract_permalink_hash(arg):
  """Extract the 40-char permalink hash from a share link or bare hash."""
  arg = arg.strip()
  if HASH_RE.match(arg):
    return arg
  # The share URL keeps its state in the fragment, e.g.
  # https://ui.perfetto.dev/#!/?s=<hash>. urllib doesn't parse query args out of
  # the fragment, so sweep the whole string for `s=<hash>`.
  m = re.search(r'[?&#]s=([a-fA-F0-9]{40})', arg)
  if m:
    return m.group(1)
  raise ValueError(
      "Could not find a 40-char permalink hash in the input. Expected a share "
      "link like 'https://ui.perfetto.dev/#!/?s=<hash>' or a bare hash.")


def _fetch(url):
  # The UI uses unauthenticated GCS reads; skip cert verification to avoid
  # local trust-store issues (mirrors tools/update_permalink.py).
  context = ssl._create_unverified_context()
  return urllib.request.urlopen(url, context=context)


def resolve_trace_url(share_link):
  """Resolve a share link (or bare hash) to the underlying trace file URL."""
  permalink_hash = extract_permalink_hash(share_link)
  url = 'https://storage.googleapis.com/%s/%s' % (BUCKET_NAME, permalink_hash)
  print('Fetching permalink: %s' % url, file=sys.stderr)
  try:
    body = _fetch(url).read().decode()
  except urllib.error.HTTPError as e:
    raise ValueError('Failed to fetch permalink (HTTP %d): %s' % (e.code, url))
  try:
    state = json.loads(body)
  except json.JSONDecodeError:
    raise ValueError('Permalink is not valid JSON (does the hash exist?): %s' %
                     url)
  # New-style permalink: {traceUrl, appState}. Legacy permalink nests the URL
  # under engine.source.url.
  trace_url = state.get('traceUrl') or (state.get('engine') or {}).get(
      'source', {}).get('url')
  if not trace_url:
    raise ValueError(
        'Permalink does not contain a trace URL. The share link may have been '
        'created from a trace that was never uploaded (e.g. an HTTP/RPC '
        'source).')
  return trace_url


def download_trace(share_link, out_path=None):
  """Download the trace behind a share link. Returns the output path."""
  trace_url = resolve_trace_url(share_link)
  if out_path is None:
    name = os.path.basename(urllib.parse.urlparse(trace_url).path)
    out_path = name if name else 'trace.pftrace'
  print('Downloading trace: %s' % trace_url, file=sys.stderr)
  resp = _fetch(trace_url)
  total = resp.length or 0
  downloaded = 0
  with open(out_path, 'wb') as f:
    while True:
      chunk = resp.read(1 << 20)
      if not chunk:
        break
      f.write(chunk)
      downloaded += len(chunk)
      if total:
        pct = downloaded * 100 // total
        print(
            '\r  %d / %d MiB (%d%%)' % (downloaded >> 20, total >> 20, pct),
            end='',
            file=sys.stderr)
  print('', file=sys.stderr)
  print('Saved %d bytes to %s' % (downloaded, out_path), file=sys.stderr)
  return out_path


def main():
  parser = argparse.ArgumentParser(
      description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument(
      'share_link', help='A ui.perfetto.dev share link or a bare hash.')
  parser.add_argument('-o', '--output', help='Output file path.')
  parser.add_argument(
      '--print-url',
      action='store_true',
      help="Only print the resolved trace URL, don't download.")
  args = parser.parse_args()

  try:
    if args.print_url:
      print(resolve_trace_url(args.share_link))
    else:
      download_trace(args.share_link, args.output)
  except ValueError as e:
    print('Error: %s' % e, file=sys.stderr)
    return 1
  return 0
