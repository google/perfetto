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
import jwt
import logging
import os
import requests
import time

from base64 import b64encode, b64decode
from datetime import datetime

from config import PROJECT, GITHUB_REPO, GITHUB_APP_ID, GITHUB_APP_INSTALLATION_ID

# Thread pool for making http requests asynchronosly.
thread_pool = concurrent.futures.ThreadPoolExecutor(max_workers=8)

# Caller has to initialize this
SCOPES = []
cached_gerrit_creds = None
cached_oauth2_creds = None

GITHUB_API_URL = 'https://api.github.com'


class ConcurrentModificationError(Exception):
  pass


def get_access_token():
  """Returns the access token for the current service account"""
  global cached_oauth2_creds
  creds = cached_oauth2_creds
  if creds is None or not creds.valid or creds.expired:
    creds, _project = google.auth.default(scopes=SCOPES)
    request = google.auth.transport.requests.Request()
    creds.refresh(request)
    cached_oauth2_creds = creds
  return creds.token


def get_secret(secret_id):
  """Retrieves a secret stored in the Cloud Secrets API"""
  access_token = get_access_token()
  url = f'https://secretmanager.googleapis.com/v1/projects/{PROJECT}/secrets/{secret_id}/versions/latest:access'
  headers = {'Authorization': f'Bearer {access_token}'}
  response = requests.get(url, headers=headers)
  response.raise_for_status()
  return b64decode(response.json()['payload']['data'])


def get_github_installation_token():
  gh_private_key = get_secret('perfetto_ci_github_private_key')
  now = int(time.time())
  payload = {
      'iat': now,
      'exp': now + (10 * 60),  # JWT valid for 10 minutes
      'iss': GITHUB_APP_ID
  }
  jwt_token = jwt.encode(payload, gh_private_key, algorithm='RS256')
  url = f'{GITHUB_API_URL}/app/installations/{GITHUB_APP_INSTALLATION_ID}/access_tokens'
  headers = {
      'Authorization': f'Bearer {jwt_token}',
      'Accept': 'application/vnd.github.v3+json'
  }
  response = requests.post(url, headers=headers)
  response.raise_for_status()
  return response.json()['token']


def get_github_registration_token():
  inst_token = get_github_installation_token()
  url = f'{GITHUB_API_URL}/repos/{GITHUB_REPO}/actions/runners/registration-token'
  headers = {
      'Authorization': f'token {inst_token}',
      'Accept': 'application/vnd.github.v3+json'
  }
  response = requests.post(url, headers=headers)
  response.raise_for_status()
  return response.json()['token']


async def req_async(method, url, body=None):
  loop = asyncio.get_running_loop()
  # run_in_executor cannot take kwargs, we need to stick with order.
  return await loop.run_in_executor(thread_pool, req, method, url, body, False,
                                    None)


def req(method, url, body=None, req_etag=False, etag=None):
  '''Helper function to handle authenticated HTTP requests.

  Cloud API and Gerrit require two different types of authentication and as
  such need to be handled differently. The HTTP connection is cached in the
  TLS slot to avoid refreshing oauth tokens too often for back-to-back requests.
  Appengine takes care of clearing the TLS slot upon each frontend request so
  these connections won't be recycled for too long.
  '''
  hdr = {'Content-Type': 'application/json; charset=UTF-8'}
  if SCOPES:
    auth_header = 'Bearer ' + get_access_token()
  logging.debug('%s %s', method, url)
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
    res = json.loads(res)
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
