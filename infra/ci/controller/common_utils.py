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

import asyncio
import concurrent.futures
import google.auth
import google.auth.transport.requests
import json
import logging
import os
import requests

from base64 import b64encode
from datetime import datetime
from config import PROJECT

# Thread pool for making http requests asynchronosly.
thread_pool = concurrent.futures.ThreadPoolExecutor(max_workers=8)

# Caller has to initialize this
SCOPES = []
cached_gerrit_creds = None
cached_oauth2_creds = None


class ConcurrentModificationError(Exception):
  pass


def get_access_token():
  global cached_oauth2_creds
  creds = cached_oauth2_creds
  if creds is None or not creds.valid or creds.expired:
    creds, _project = google.auth.default(scopes=SCOPES)
    request = google.auth.transport.requests.Request()
    creds.refresh(request)
    cached_oauth2_creds = creds
  return creds.token


def get_gerrit_credentials():
  '''Retrieve the credentials used to authenticate Gerrit requests

  Returns a tuple (user, gitcookie). These fields are obtained from the Gerrit
  'New HTTP password' page which generates a .gitcookie file and stored in the
  project datastore.
  user: typically looks like git-user.gmail.com.
  gitcookie: is the password after the = token.
  '''
  global cached_gerrit_creds
  if cached_gerrit_creds is None:
    body = {'query': {'kind': [{'name': 'GerritAuth'}]}}
    res = req(
        'POST',
        'https://datastore.googleapis.com/v1/projects/%s:runQuery' % PROJECT,
        body=body)
    auth = res['batch']['entityResults'][0]['entity']['properties']
    user = auth['user']['stringValue']
    gitcookie = auth['gitcookie']['stringValue']
    cached_gerrit_creds = user, gitcookie
  return cached_gerrit_creds


async def req_async(method, url, body=None, gerrit=False):
  loop = asyncio.get_running_loop()
  # run_in_executor cannot take kwargs, we need to stick with order.
  return await loop.run_in_executor(thread_pool, req, method, url, body, gerrit,
                                    False, None)


def req(method, url, body=None, gerrit=False, req_etag=False, etag=None):
  '''Helper function to handle authenticated HTTP requests.

  Cloud API and Gerrit require two different types of authentication and as
  such need to be handled differently. The HTTP connection is cached in the
  TLS slot to avoid refreshing oauth tokens too often for back-to-back requests.
  Appengine takes care of clearing the TLS slot upon each frontend request so
  these connections won't be recycled for too long.
  '''
  hdr = {'Content-Type': 'application/json; charset=UTF-8'}
  if gerrit:
    creds = '%s:%s' % get_gerrit_credentials()
    auth_header = 'Basic ' + b64encode(creds.encode('utf-8')).decode('utf-8')
  elif SCOPES:
    auth_header = 'Bearer ' + get_access_token()
  logging.debug('%s %s [gerrit=%d]', method, url, gerrit)
  hdr['Authorization'] = auth_header
  if req_etag:
    hdr['X-Firebase-ETag'] = 'true'
  if etag:
    hdr['if-match'] = etag
  body = None if body is None else json.dumps(body)
  resp = requests.request(method, url, headers=hdr, data=body, timeout=60)
  res = resp.content.decode('utf-8')
  resp_etag = resp.headers.get('etag')
  if resp.status_code == 200:
    # [4:] is to strip Gerrit XSSI projection prefix.
    res = json.loads(res[4:] if gerrit else res)
    return (res, resp_etag) if req_etag else res
  elif resp.status_code == 412:
    raise ConcurrentModificationError()
  else:
    raise Exception(resp, res)


# Datetime functions to deal with the fact that Javascript expects a trailing
# 'Z' (Z == 'Zulu' == UTC) for timestamps.
def parse_iso_time(time_str):
  return datetime.strptime(time_str, r'%Y-%m-%dT%H:%M:%SZ')


def utc_now_iso(utcnow=None):
  return (utcnow or datetime.utcnow()).strftime(r'%Y-%m-%dT%H:%M:%SZ')


def defer(coro):
  loop = asyncio.get_event_loop()
  task = loop.create_task(coro)
  task.set_name(coro.cr_code.co_name)
  return task


def init_logging():
  logging.basicConfig(
      format='%(levelname)-8s %(asctime)s %(message)s',
      level=logging.DEBUG if os.getenv('VERBOSE') else logging.INFO,
      datefmt=r'%Y-%m-%d %H:%M:%S')
