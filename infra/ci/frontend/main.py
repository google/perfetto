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
import json

from collections import namedtuple
from config import GITHUB_REPO, PROJECT
from common_utils import SCOPES, get_github_installation_token, req_async, utc_now_iso
from stackdriver_metrics import STACKDRIVER_METRICS
''' Makes anonymous GET-only requests to Gerrit.

Solves the lack of CORS headers from AOSP gerrit.
'''

STACKDRIVER_API = 'https://monitoring.googleapis.com/v3/projects/%s' % PROJECT

SCOPES.append('https://www.googleapis.com/auth/cloud-platform')
# SCOPES.append('https://www.googleapis.com/auth/userinfo.email')
SCOPES.append('https://www.googleapis.com/auth/datastore')
SCOPES.append('https://www.googleapis.com/auth/monitoring')
SCOPES.append('https://www.googleapis.com/auth/monitoring.write')

HASH_RE = re.compile('^[a-f0-9]+$')
CACHE_TTL = 3600  # 1 h
CacheEntry = namedtuple('CacheEntry', ['contents', 'expiration'])

app = flask.Flask(__name__)

logging.basicConfig(
    format='%(levelname)-8s %(asctime)s %(message)s',
    level=logging.DEBUG if os.getenv('VERBOSE') else logging.INFO,
    datefmt=r'%Y-%m-%d %H:%M:%S')

_cached_gh_token = None
_cached_gh_token_expiry = 0  # Epoch timestamp

HEX_PATTERN = re.compile(r'^[0-9a-fA-F]+$')


def get_cached_github_token():
  global _cached_gh_token, _cached_gh_token_expiry
  now = time.time()
  if _cached_gh_token is None or now >= _cached_gh_token_expiry:
    _cached_gh_token = get_github_installation_token()
    _cached_gh_token_expiry = now + 3600  # Cache for 1 hour
  return _cached_gh_token


def gh_req(url, headers={}, params={}):
  headers.update({
      'Authorization': f'token {get_cached_github_token()}',
      'Accept': 'application/vnd.github+json'
  })
  resp = requests.get(url, headers=headers, params=params)
  return resp.content.decode('utf-8')


@app.route('/_ah/start', methods=['GET', 'POST'])
async def http_start():
  await create_stackdriver_metric_definitions()
  return 'OK'


async def create_stackdriver_metric_definitions():
  logging.info('Creating Stackdriver metric definitions')
  for name, metric in STACKDRIVER_METRICS.items():
    logging.info('Creating metric %s', name)
    await req_async('POST', STACKDRIVER_API + '/metricDescriptors', body=metric)


@app.route('/gh/pulls')
async def gh_pulls():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/pulls'
  params = {
      'state': 'all',
      'per_page': 50,
      'sort': 'updated',
      'direction': 'desc'
  }
  return gh_req(url, params=params)


@app.route('/gh/checks/<string:sha>')
async def gh_checks(sha):
  if not HEX_PATTERN.fullmatch(sha):
    flask.abort(400, description="Invalid hex string")
  url = f'https://api.github.com/repos/{GITHUB_REPO}/commits/{sha}/check-runs'
  return gh_req(url)


@app.route('/gh/patchsets/<int:pr>')
async def gh_patchsets(pr):
  url = f'https://api.github.com/repos/{GITHUB_REPO}/pulls/{pr}/commits'
  return gh_req(url)


@app.route('/gh/commits/main')
async def gh_commits_main():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/commits'
  params = {'sha': 'main'}
  return gh_req(url, params=params)


@app.route('/gh/runners')
async def gh_runners():
  params = {'per_page': 100}
  url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runners'
  return gh_req(url, params=params)


async def get_jobs_for_workflow_run(id):
  url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runs/{id}/jobs'
  resp = gh_req(url, params={'per_page': 100})
  respj = json.loads(resp)
  jobs = []
  for job in respj['jobs']:
    jobs.append({
        'id': job.get('id'),
        'name': job.get('name'),
        'html_url': job.get('html_url'),
        'status': job.get('status'),
        'conclusion': job.get('conclusion'),
        'created_at': job.get('created_at'),
        'updated_at': job.get('updated_at'),
        'completed_at': job.get('completed_at'),
        'runner_id': job.get('runner_id'),
        'runner_name': job.get('runner_name'),
    })
  return jobs


@app.route('/gh/workflows')
async def workflow_runs_and_jobs():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runs'
  resp = gh_req(url, params={'per_page': 100})
  respj = json.loads(resp)
  runs = []
  for run in respj['workflow_runs']:
    if run.get('name') != 'Perfetto CI':
      continue
    resp_obj = {
        'id': run.get('id'),
        'event': run.get('event'),
        'display_title': run.get('display_title'),
        'html_url': run.get('html_url'),
        'head_sha': run.get('head_sha'),
        'status': run.get('status'),
        'conclusion': run.get('conclusion'),
        'login': run.get('actor', {}).get('login'),
        'created_at': run.get('created_at'),
        'updated_at': run.get('updated_at'),
        'run_started_at': run.get('run_started_at'),
        'jobs': await get_jobs_for_workflow_run(run.get('id'))
    }
    runs.append(resp_obj)
  return runs


@app.route('/gh/update_metrics')
async def update_metrics():
  url = f'https://api.github.com/repos/{GITHUB_REPO}/actions/runs'
  resp = gh_req(url, params={'per_page': 100})
  txt = resp.content.decode('utf-8')
  respj = json.loads(txt)
  num_pending = 0
  for w in respj.get('workflow_runs', []):
    if w['status'] in ('queued', 'in_progress'):
      num_pending += 1
  await write_metrics({'ci_job_queue_len': {'v': num_pending}})
  return str(num_pending)


async def write_metrics(metric_dict):
  now = utc_now_iso()
  desc = {'timeSeries': []}
  for key, spec in metric_dict.items():
    desc['timeSeries'] += [{
        'metric': {
            'type': STACKDRIVER_METRICS[key]['type'],
            'labels': spec.get('l', {})
        },
        'resource': {
            'type': 'global'
        },
        'points': [{
            'interval': {
                'endTime': now
            },
            'value': {
                'int64Value': str(spec['v'])
            }
        }]
    }]
  try:
    await req_async('POST', STACKDRIVER_API + '/timeSeries', body=desc)
  except Exception as e:
    # Metric updates can easily fail due to Stackdriver API limitations.
    msg = str(e)
    if 'written more frequently than the maximum sampling' not in msg:
      logging.error('Metrics update failed: %s', msg)
