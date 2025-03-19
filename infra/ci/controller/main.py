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
import flask
import logging
import requests
import json

from datetime import datetime, timedelta
from common_utils import init_logging, defer, req_async, utc_now_iso, parse_iso_time, SCOPES
from config import PROJECT, GITHUB_REPO
from functools import wraps
from stackdriver_metrics import STACKDRIVER_METRICS

STACKDRIVER_API = 'https://monitoring.googleapis.com/v3/projects/%s' % PROJECT

SCOPES.append('https://www.googleapis.com/auth/cloud-platform')
SCOPES.append('https://www.googleapis.com/auth/firebase.database')
SCOPES.append('https://www.googleapis.com/auth/userinfo.email')
SCOPES.append('https://www.googleapis.com/auth/datastore')
SCOPES.append('https://www.googleapis.com/auth/monitoring')
SCOPES.append('https://www.googleapis.com/auth/monitoring.write')

app = flask.Flask(__name__)

is_handling_route = {}

# ------------------------------------------------------------------------------
# Misc utility functions
# ------------------------------------------------------------------------------


def no_concurrency(f):
  route_name = f.__name__
  is_handling_route[route_name] = False

  @wraps(f)
  async def decorated_function(*args, **kwargs):
    if is_handling_route[route_name]:
      return flask.abort(
          423, description='Handler %s already running' % route_name)
    is_handling_route[route_name] = True
    try:
      return await f(*args, **kwargs)
    finally:
      is_handling_route[route_name] = False

  return decorated_function


# ------------------------------------------------------------------------------
# HTTP handlers
# ------------------------------------------------------------------------------


@app.route('/_ah/start', methods=['GET', 'POST'])
async def http_start():
  init_logging()
  await create_stackdriver_metric_definitions()
  return 'OK ' + datetime.now().isoformat()


@app.route('/controller/tick', methods=['GET', 'POST'])
@no_concurrency
async def http_tick():
  # The tick is invoked by cron.yaml every 1 minute, it doesn't allow sub-minute
  # jobs. Here we want to poll every 30 seconds to be more responsive. So every
  # tick keeps repeating the polling for a minute.
  deadline = datetime.now() + timedelta(seconds=55)
  while datetime.now() < deadline:
    await update_queue_metrics()
    asyncio.sleep(30)
  return 'OK ' + datetime.now().isoformat()


async def update_queue_metrics():
  # Update the stackdriver metric that will drive the autoscaler.
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
  await write_metrics({'ci_job_queue_len': {'v': num_pending}})


async def create_stackdriver_metric_definitions():
  logging.info('Creating Stackdriver metric definitions')
  for name, metric in STACKDRIVER_METRICS.items():
    logging.info('Creating metric %s', name)
    await req_async('POST', STACKDRIVER_API + '/metricDescriptors', body=metric)


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
