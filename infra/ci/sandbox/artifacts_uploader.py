#!/usr/bin/env python3
# Copyright (C) 2019 The Android Open Source Project
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
import httplib2
import logging
import mimetypes
import mmap
import os
import subprocess
import signal
import sys
import threading
import time

from config import GCS_ARTIFACTS

from multiprocessing.pool import ThreadPool

CUR_DIR = os.path.dirname(__file__)
WATCHDOG_SEC = 60 * 6  # Self kill after 5 minutes

tls = threading.local()
'''Polls for new directories under ARTIFACTS_DIR and uploads them to GCS'''


def get_http_obj():
  http = getattr(tls, 'http', None)
  if http is not None:
    return http
  token_path = os.environ.get('SVC_TOKEN_PATH')
  if token_path is None:
    raise EnvironmentError('SVC_TOKEN_PATH must be set to upload to GCS')
  with open(token_path, 'r') as f:
    token = f.read()
  http = httplib2.Http()
  reqf = http.request

  def request_with_bearer(uri, method="GET", body=None, headers=None, **kwargs):
    headers = headers or {}
    headers['Authorization'] = f'Bearer {token}'
    return reqf(uri, method=method, body=body, headers=headers, **kwargs)

  http.request = request_with_bearer
  tls.http = http
  return http


def upload_one_file(fpath, base):
  http = get_http_obj()
  relpath = os.path.relpath(fpath, base)
  logging.debug('Uploading %s', relpath)
  assert (os.path.exists(fpath))
  fsize = os.path.getsize(fpath)
  mime_type = mimetypes.guess_type(fpath)[0] or 'application/octet-stream'
  mm = ''
  hdr = {'Content-Length': str(fsize), 'Content-type': mime_type}
  if fsize > 0:
    with open(fpath, 'rb') as f:
      mm = mmap.mmap(f.fileno(), fsize, access=mmap.ACCESS_READ)
  uri = 'https://%s.storage.googleapis.com/%s' % (GCS_ARTIFACTS, relpath)
  resp, res = http.request(uri, method='PUT', headers=hdr, body=mm)
  if fsize > 0:
    mm.close()
  if resp.status != 200:
    logging.error('HTTP request failed with code %d : %s', resp.status, res)
    return -1
  return fsize


def upload_one_file_with_retries(fpath, base):
  for retry in [0.5, 1.5, 3]:
    res = upload_one_file(fpath, base)
    if res >= 0:
      return res
    logging.warning('Upload of %s failed, retrying in %s seconds', fpath, retry)
    time.sleep(retry)
  return -1


def list_files(path):
  for root, _, files in os.walk(path):
    for fname in files:
      fpath = os.path.join(root, fname)
      if os.path.isfile(fpath):
        yield fpath


def main():
  logging.basicConfig(
      format='%(levelname)-8s %(asctime)s %(message)s',
      level=logging.DEBUG if os.getenv('VERBOSE') else logging.INFO,
      datefmt=r'%Y-%m-%d %H:%M:%S')
  signal.alarm(WATCHDOG_SEC)
  mimetypes.add_type('application/wasm', '.wasm')

  parser = argparse.ArgumentParser()
  parser.add_argument('--rm', action='store_true', help='Removes the directory')
  parser.add_argument(
      'dir', type=str, help='The directory containing the artifacts')
  args = parser.parse_args()
  dirpath = args.dir
  if not os.path.isdir(dirpath):
    logging.error('Directory not found: %s', dirpath)
    return 1

  total_size = 0
  uploads = 0
  failures = 0
  files = list_files(dirpath)
  pool = ThreadPool(processes=10)
  upload_fn = lambda x: upload_one_file_with_retries(x, dirpath)
  for upl_size in pool.imap_unordered(upload_fn, files):
    uploads += 1 if upl_size >= 0 else 0
    failures += 1 if upl_size < 0 else 0
    total_size += max(upl_size, 0)

  logging.info('Uploaded artifacts for %s: %d files, %s failures, %d KB',
               dirpath, uploads, failures, total_size / 1e3)

  if args.rm:
    subprocess.call(['rm', '-rf', dirpath])

  return 0


if __name__ == '__main__':
  sys.exit(main())
