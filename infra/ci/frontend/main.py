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

import flask
import json
import logging
import os
import re
import requests
import time

from collections import namedtuple
from config import GERRIT_HOST, GERRIT_PROJECT, GITHUB_REPO
from common_utils import get_github_installation_token, SCOPES

SCOPES.append('https://www.googleapis.com/auth/cloud-platform')

''' Makes anonymous GET-only requests to Gerrit.

Solves the lack of CORS headers from AOSP gerrit.
'''

HASH_RE = re.compile('^[a-f0-9]+$')
CACHE_TTL = 3600  # 1 h
CacheEntry = namedtuple('CacheEntry', ['contents', 'expiration'])

app = flask.Flask(__name__)

logging.basicConfig(
    format='%(levelname)-8s %(asctime)s %(message)s',
    level=logging.DEBUG if os.getenv('VERBOSE') else logging.INFO,
    datefmt=r'%Y-%m-%d %H:%M:%S')

cache = {}


def DeleteStaleCacheEntries():
  now = time.time()
  for url, entry in list(cache.items()):
    if now > entry.expiration:
      cache.pop(url, None)


def req_cached(url):
  '''Used for requests that return immutable data, avoid hitting Gerrit 500'''
  DeleteStaleCacheEntries()
  entry = cache.get(url)
  contents = entry.contents if entry is not None else None
  if not contents:
    resp = requests.get(url)
    if resp.status_code != 200:
      err_str = 'http error %d while fetching %s' % (resp.status_code, url)
      return resp.status_code, err_str
    contents = resp.content.decode('utf-8')
    cache[url] = CacheEntry(contents, time.time() + CACHE_TTL)
  return contents, 200


@app.route('/gh/', methods=['GET', 'POST'])
def gerrit_changes():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runs'
  params = {'per_page': 1000}
  headers = {
      'Authorization': f'token {get_github_installation_token()}',
      'Accept': 'application/vnd.github+json'
  }
  resp = requests.get(url, headers=headers, params=params)
  respj = json.loads(resp.content.decode('utf-8'))
  num_pending = 0
  for w in respj.get('workflow_runs', []):
    if w['status'] in ('queued', 'in_progress'):
      num_pending += 1
  hdr = {'Content-Type': 'text/plain'}
  status = resp.status_code
  return str(num_pending), status, hdr
