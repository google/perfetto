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
import logging
import os
import re
import requests
import time
import urllib.parse

from collections import namedtuple
from config import GERRIT_HOST, GERRIT_PROJECT
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


@app.route('/gerrit/commits/<string:sha1>', methods=['GET', 'POST'])
def commits(sha1):
  if not HASH_RE.match(sha1):
    return 'Malformed input', 500
  project = urllib.parse.quote(GERRIT_PROJECT, '')
  url = 'https://%s/projects/%s/commits/%s' % (GERRIT_HOST, project, sha1)
  content, status = req_cached(url)
  return content[4:], status  # 4: -> Strip Gerrit XSSI chars.


@app.route(
    '/gerrit/log/<string:first>..<string:second>', methods=['GET', 'POST'])
def gerrit_log(first, second):
  if not HASH_RE.match(first) or not HASH_RE.match(second):
    return 'Malformed input', 500
  url = 'https://%s/%s/+log/%s..%s?format=json' % (GERRIT_HOST.replace(
      '-review', ''), GERRIT_PROJECT, first, second)
  content, status = req_cached(url)
  return content[4:], status  # 4: -> Strip Gerrit XSSI chars.


@app.route('/gerrit/changes/', methods=['GET', 'POST'])
def gerrit_changes():
  url = 'https://%s/changes/?q=project:%s+' % (GERRIT_HOST, GERRIT_PROJECT)
  url += flask.request.query_string.decode('utf-8')
  resp = requests.get(url)
  hdr = {'Content-Type': 'text/plain'}
  status = resp.status_code
  if status == 200:
    resp = resp.content.decode('utf-8')[4:]  # 4: -> Strip Gerrit XSSI chars.
  else:
    resp = 'HTTP error %s' % status
  return resp, status, hdr
