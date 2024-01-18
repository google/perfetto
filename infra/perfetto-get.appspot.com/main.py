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

import base64
import requests
import time

from collections import namedtuple
from flask import Flask, make_response, redirect

BASE = 'https://android.googlesource.com/platform/external/perfetto.git/' \
       '+/main/%s?format=TEXT'

RESOURCES = {
    'tracebox': 'tools/tracebox',
    'traceconv': 'tools/traceconv',
    'trace_processor': 'tools/trace_processor',
}

CACHE_TTL = 3600  # 1 h

CacheEntry = namedtuple('CacheEntry', ['contents', 'expiration'])
cache = {}

app = Flask(__name__)


def DeleteStaleCacheEntries():
  now = time.time()
  for url, entry in list(cache.items()):
    if now > entry.expiration:
      cache.pop(url, None)


@app.route('/')
def root():
  return redirect('https://www.perfetto.dev/', code=301)


@app.route('/<string:resource>')
def fetch_artifact(resource):
  hdrs = {'Content-Type': 'text/plain'}
  resource = resource.lower()
  if resource not in RESOURCES:
    return make_response('Resource "%s" not found' % resource, 404, hdrs)
  url = BASE % RESOURCES[resource]
  DeleteStaleCacheEntries()
  entry = cache.get(url)
  contents = entry.contents if entry is not None else None
  if not contents:
    req = requests.get(url)
    if req.status_code != 200:
      err_str = 'http error %d while fetching %s' % (req.status_code, url)
      return make_response(err_str, req.status_code, hdrs)
    contents = base64.b64decode(req.text)
    cache[url] = CacheEntry(contents, time.time() + CACHE_TTL)
  hdrs = {'Content-Disposition': 'attachment; filename="%s"' % resource}
  return make_response(contents, 200, hdrs)
