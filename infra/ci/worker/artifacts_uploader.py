#!/usr/bin/env python2
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

import httplib2
import logging
import mimetypes
import mmap
import os
import subprocess
import sys
import threading
import time

from common_utils import init_logging
from config import GCS_ARTIFACTS
from multiprocessing.pool import ThreadPool
from oauth2client.client import GoogleCredentials

RESCAN_PERIOD_SEC = 5  # Scan for new artifact directories every X seconds.

tls = threading.local()

'''Polls for new directories under ARTIFACTS_DIR and uploads them to GCS'''


def get_http_obj():
  http = getattr(tls, 'http', None)
  if http is not None:
    return http
  tls.http = httplib2.Http()
  scopes = ['https://www.googleapis.com/auth/cloud-platform']
  creds = GoogleCredentials.get_application_default().create_scoped(scopes)
  creds.authorize(tls.http)
  return tls.http


def upload_one_file(fpath):
  http = get_http_obj()
  relpath = os.path.relpath(fpath, os.getenv('ARTIFACTS_DIR'))
  logging.debug('Uploading %s', relpath)
  fsize = os.path.getsize(fpath)
  mime_type = mimetypes.guess_type(fpath)[0] or 'application/octet-stream'
  mm = ''
  hdr = {'Content-Length': fsize, 'Content-type': mime_type}
  if fsize > 0:
    with open(fpath, 'rb') as f:
      mm = mmap.mmap(f.fileno(), fsize, access=mmap.ACCESS_READ)
  uri = 'https://%s.storage.googleapis.com/%s' % (GCS_ARTIFACTS, relpath)
  resp, _ = http.request(uri, method='PUT', headers=hdr, body=mm)
  if fsize > 0:
    mm.close()
  return fsize if resp.status == 200 else -1


def list_files(path):
  for root, _, files in os.walk(path):
    for fname in files:
      fpath = os.path.join(root, fname)
      if os.path.isfile(fpath):
        yield fpath


def scan_and_uplod_artifacts(pool, remove_after_upload=False):
  root = os.getenv('ARTIFACTS_DIR')
  for job_id in (x for x in os.listdir(root) if not x.endswith('.tmp')):
    dirpath = os.path.join(root, job_id)
    if not os.path.isdir(dirpath):
      continue
    logging.debug('Uploading %s', dirpath)
    total_size = 0
    uploads = 0
    failures = 0
    for upl_size in pool.imap_unordered(upload_one_file, list_files(dirpath)):
      uploads += 1 if upl_size >= 0 else 0
      failures += 1 if upl_size < 0 else 0
      total_size += max(upl_size, 0)
    logging.info('Uploaded artifacts for %s: %d files, %s failures, %d KB',
                 job_id, uploads, failures, total_size / 1e3)
    if remove_after_upload:
      subprocess.call(['sudo', 'rm', '-rf', dirpath])


def main():
  init_logging()
  mimetypes.add_type('application/wasm', '.wasm')
  logging.info('Artifacts uploader started')
  pool = ThreadPool(processes=32)
  while True:
    scan_and_uplod_artifacts(pool, remove_after_upload='--rm' in sys.argv)
    time.sleep(RESCAN_PERIOD_SEC)


if __name__ == '__main__':
  sys.exit(main())
