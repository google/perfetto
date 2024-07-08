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
import re
import urllib.parse

from datetime import datetime, timedelta
from common_utils import init_logging, defer, req_async, utc_now_iso, parse_iso_time, SCOPES
from config import DB, GERRIT_HOST, GERRIT_PROJECT, PROJECT
from config import CI_SITE, GERRIT_VOTING_ENABLED, JOB_CONFIGS, LOGS_TTL_DAYS
from config import TRUSTED_EMAILS, GCS_ARTIFACTS, JOB_TIMEOUT_SEC
from config import CL_TIMEOUT_SEC
from functools import wraps
from stackdriver_metrics import STACKDRIVER_METRICS

STACKDRIVER_API = 'https://monitoring.googleapis.com/v3/projects/%s' % PROJECT

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


def is_trusted(email):
  return re.match(TRUSTED_EMAILS, email)


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
  # jobs. Here we want to poll every 15 seconds to be more responsive. So every
  # tick keeps repeating the polling for a minute.
  deadline = datetime.now() + timedelta(seconds=55)
  while datetime.now() < deadline:
    await check_new_cls()
    await check_pending_cls()
    await update_queue_metrics()
    asyncio.sleep(15)
  return 'OK ' + datetime.now().isoformat()


@app.route('/controller/queue_postsubmit_jobs', methods=['GET', 'POST'])
@no_concurrency
async def http_queue_postsubmit_jobs():
  await queue_postsubmit_jobs('main')
  return 'OK ' + datetime.now().isoformat()


@app.route('/controller/delete_stale_jobs', methods=['GET', 'POST'])
@no_concurrency
async def http_delete_stale_jobs():
  await delete_stale_jobs()
  return 'OK ' + datetime.now().isoformat()


@app.route('/controller/delete_stale_workers', methods=['GET', 'POST'])
@no_concurrency
async def http_delete_stale_workers():
  await delete_stale_workers()
  return 'OK ' + datetime.now().isoformat()


@app.route('/controller/delete_expired_logs', methods=['GET', 'POST'])
@no_concurrency
async def http_delete_expired_logs():
  await delete_expired_logs(LOGS_TTL_DAYS)
  return 'OK ' + datetime.now().isoformat()


# Enddpoints below are only for manual testing & mainteinance.


@app.route(
    '/controller/delete_expired_logs/<int:ttl_days>', methods=['GET', 'POST'])
async def http_delete_expired_logs_ttl(ttl_days):
  await delete_expired_logs(ttl_days)
  return 'OK ' + datetime.now().isoformat()


@app.route('/controller/delete_job_logs/<job_id>', methods=['GET', 'POST'])
async def http_delete_job_logs(job_id):
  await delete_job_logs(job_id)
  return 'OK ' + datetime.now().isoformat()


# This is to test HTTP timeouts
@app.route('/controller/sleep/<int:sleep_sec>', methods=['GET', 'POST'])
async def http_sleep(sleep_sec):
  await asyncio.sleep(sleep_sec)
  return 'OK ' + datetime.now().isoformat()


@app.route('/controller/sleep_locked/<int:sleep_sec>', methods=['GET', 'POST'])
@no_concurrency
async def http_sleep_locked(sleep_sec):
  await asyncio.sleep(sleep_sec)
  return 'OK ' + datetime.now().isoformat()


# ------------------------------------------------------------------------------
# Deferred jobs
# ------------------------------------------------------------------------------


async def check_new_cls():
  ''' Poll for new CLs and asynchronously enqueue jobs for them.'''
  logging.info('Polling for new Gerrit CLs')
  date_limit = (datetime.utcnow() - timedelta(days=1)).strftime('%Y-%m-%d')
  url = 'https://%s/a/changes/' % GERRIT_HOST
  url += '?o=CURRENT_REVISION&o=DETAILED_ACCOUNTS&o=LABELS&n=32'
  url += '&q=branch:main+project:%s' % GERRIT_PROJECT
  url += '+is:open+after:%s' % date_limit
  resp = await req_async('GET', url, gerrit=True)
  tasks = []
  for change in (change for change in resp if 'revisions' in change):
    rev_hash = list(change['revisions'].keys())[0]
    rev = change['revisions'][rev_hash]
    owner = rev['uploader']['email']
    prs_ready = change['labels'].get('Presubmit-Ready', {}).get('approved', {})
    prs_owner = prs_ready.get('email', '')
    # Only submit jobs for patchsets that are either uploaded by a trusted
    # account or are marked as Presubmit-Verified by a trustd account.
    if not is_trusted(owner) and not is_trusted(prs_owner):
      continue
    tasks.append(
        defer(
            check_new_cl(
                cl=str(change['_number']),
                patchset=str(rev['_number']),
                change_id=change['id'],
                rev_hash=rev_hash,
                ref=rev['ref'],
                wants_vote=True if prs_ready else False)))
  await asyncio.gather(*tasks)


def append_jobs(patch_obj, src, git_ref, now=None):
  '''Creates the worker jobs (defined in config.py) for the given CL.

  Jobs are keyed by timestamp-cl-patchset-config to get a fair schedule (workers
  pull jobs ordered by the key above).
  It dosn't directly write into the DB, it just appends keys to the passed
  |patch_obj|, so the whole set of CL descriptor + jobs can be added atomically
  to the datastore.
  src: is cls/1234/1 (cl and patchset number).
  '''
  logging.info('Enqueueing jobs fos cl %s', src)
  timestamp = (now or datetime.utcnow()).strftime('%Y%m%d%H%M%S')
  for cfg_name, env in JOB_CONFIGS.items():
    job_id = '%s--%s--%s' % (timestamp, src.replace('/', '-'), cfg_name)
    logging.info('Enqueueing job %s', job_id)
    patch_obj['jobs/' + job_id] = {
        'src': src,
        'type': cfg_name,
        'env': dict(env, PERFETTO_TEST_GIT_REF=git_ref),
        'status': 'QUEUED',
        'time_queued': utc_now_iso(),
    }
    patch_obj['jobs_queued/' + job_id] = 0
    patch_obj[src]['jobs'][job_id] = 0


async def check_new_cl(change_id: str, rev_hash: str, cl: str, patchset: str,
                       ref: str, wants_vote: bool):
  '''Creates the CL + jobs entries in the DB for the given CL if doesn't exist

  If exists check if a Presubmit-Ready label has been added and if so updates it
  with the message + vote.
  '''
  # We want to do two things here:
  # 1) If the CL doesn't exist (hence vote_prop is None) carry on below and
  #    enqueue jobs for it.
  # 2) If the CL exists, we don't need to kick new jobs. However, the user
  #    might have addeed a Presubmit-Ready label after we created the CL. In
  #    this case update the |wants_vote| flag and return.
  logging.debug('check_new_cl(%s-%s)', cl, patchset)
  vote_prop = await req_async(
      'GET', '%s/cls/%s-%s/wants_vote.json' % (DB, cl, patchset))
  if vote_prop is not None:
    if vote_prop != wants_vote and wants_vote:
      logging.info('Updating wants_vote flag on %s-%s', cl, patchset)
      await req_async(
          'PUT', '%s/cls/%s-%s/wants_vote.json' % (DB, cl, patchset), body=True)
      # If the label is applied after we have finished running all the jobs just
      # jump straight to the voting.
      await check_pending_cl(cl_and_ps='%s-%s' % (cl, patchset))
    logging.debug('check_new_cl(%s-%s): already queued', cl, patchset)
    return

  # This is the first time we see this patchset, enqueue jobs for it.

  # Dequeue jobs for older patchsets, if any.
  await cancel_older_jobs(cl=cl, patchset=patchset)

  src = 'cls/%s-%s' % (cl, patchset)
  # Enqueue jobs for the latest patchset.
  patch_obj = {}
  patch_obj['cls_pending/%s-%s' % (cl, patchset)] = 0
  patch_obj[src] = {
      'change_id': change_id,
      'revision_id': rev_hash,
      'time_queued': utc_now_iso(),
      'jobs': {},
      'wants_vote': wants_vote,
  }
  append_jobs(patch_obj, src, ref)
  logging.debug('check_new_cl(%s-%s): queueing jobs', cl, patchset)
  await req_async('PATCH', DB + '.json', body=patch_obj)


async def cancel_older_jobs(cl: str, patchset: str):
  first_key = '%s-0' % cl
  last_key = '%s-z' % cl
  filt = 'orderBy="$key"&startAt="%s"&endAt="%s"' % (first_key, last_key)
  cl_objs = await req_async('GET', '%s/cls.json?%s' % (DB, filt)) or {}
  tasks = []
  for cl_and_ps, cl_obj in cl_objs.items():
    ps = int(cl_and_ps.split('-')[-1])
    if cl_obj.get('time_ended') or ps >= int(patchset):
      continue
    logging.info('Cancelling jobs for previous patchset %s', cl_and_ps)
    for job_id in cl_obj['jobs'].keys():
      tasks.append(defer(cancel_job(job_id=job_id)))
  await asyncio.gather(*tasks)


async def check_pending_cls():
  # Check if any pending CL has completed (all jobs are done). If so publish
  # the comment and vote on the CL.
  pending_cls = await req_async('GET', '%s/cls_pending.json' % DB) or {}
  tasks = []
  for cl_and_ps, _ in pending_cls.items():
    tasks.append(defer(check_pending_cl(cl_and_ps=cl_and_ps)))
  await asyncio.gather(*tasks)


async def check_pending_cl(cl_and_ps: str):
  # This function can be called twice on the same CL, e.g., in the case when the
  # Presubmit-Ready label is applied after we have finished running all the
  # jobs (we run presubmit regardless, only the voting is conditioned by PR).
  cl_obj = await req_async('GET', '%s/cls/%s.json' % (DB, cl_and_ps))
  all_jobs = cl_obj.get('jobs', {}).keys()
  pending_jobs = []
  interrupted_jobs = []
  for job_id in all_jobs:
    job_status = await req_async('GET', '%s/jobs/%s/status.json' % (DB, job_id))
    pending_jobs += [job_id] if job_status in ('QUEUED', 'STARTED') else []
    interrupted_jobs += [job_id] if job_status in ('INTERRUPTED') else []

  # Interrupted jobs are due to VMs being shutdown (usually due to a scale-down)
  # Automatically re-queue them so they get picked up by some other vm.
  await asyncio.gather(*[requeue_job(job_id) for job_id in interrupted_jobs])

  if pending_jobs:
    # If the CL has been pending for too long cancel all its jobs. Upon the next
    # scan it will be deleted and optionally voted on.
    t_queued = parse_iso_time(cl_obj['time_queued'])
    age_sec = (datetime.utcnow() - t_queued).total_seconds()
    if age_sec > CL_TIMEOUT_SEC:
      logging.warning('Canceling %s, it has been pending for too long (%s sec)',
                      cl_and_ps, int(age_sec))
      tasks = [defer(cancel_job(job_id)) for job_id in pending_jobs]
      await asyncio.gather(*tasks)

  if pending_jobs or interrupted_jobs:
    return
  logging.info('All jobs completed for CL %s', cl_and_ps)

  # Remove the CL from the pending queue and update end time.
  patch_obj = {
      'cls_pending/%s' % cl_and_ps: {},  # = DELETE
      'cls/%s/time_ended' % cl_and_ps: cl_obj.get('time_ended', utc_now_iso()),
  }
  await req_async('PATCH', '%s.json' % DB, body=patch_obj)
  await update_cl_metrics(src='cls/' + cl_and_ps)
  tasks = [defer(update_job_metrics(job_id)) for job_id in all_jobs]
  await asyncio.gather(*tasks)
  if cl_obj.get('wants_vote'):
    await comment_and_vote_cl(cl_and_ps=cl_and_ps)


async def comment_and_vote_cl(cl_and_ps: str):
  cl_obj = await req_async('GET', '%s/cls/%s.json' % (DB, cl_and_ps))

  if cl_obj.get('voted'):
    logging.error('Already voted on CL %s', cl_and_ps)
    return

  if not cl_obj['wants_vote'] or not GERRIT_VOTING_ENABLED:
    logging.info('Skipping voting on CL %s', cl_and_ps)
    return

  cl_vote = 1
  passed_jobs = []
  failed_jobs = {}
  ui_links = []
  cancelled = False
  for job_id in cl_obj['jobs'].keys():
    job_obj = await req_async('GET', '%s/jobs/%s.json' % (DB, job_id))
    job_config = JOB_CONFIGS.get(job_obj['type'], {})
    if job_obj['status'] == 'CANCELLED':
      cancelled = True
    if '-ui-' in job_id:
      ui_links.append('https://storage.googleapis.com/%s/%s/ui/index.html' %
                      (GCS_ARTIFACTS, job_id))
      ui_links.append(
          'https://storage.googleapis.com/%s/%s/ui-test-artifacts/index.html' %
          (GCS_ARTIFACTS, job_id))
    if job_obj['status'] == 'COMPLETED':
      passed_jobs.append(job_id)
    elif not job_config.get('SKIP_VOTING', False):
      cl_vote = -1
      failed_jobs[job_id] = job_obj['status']

  msg = ''
  if cancelled:
    msg += 'Some jobs in this CI run were cancelled. This likely happened '
    msg += 'because a new patchset has been uploaded. Skipping vote.\n'
  log_url = CI_SITE + '/#!/logs'
  if failed_jobs:
    msg += 'FAIL:\n'
    msg += ''.join([
        '- %s/%s (%s)\n' % (log_url, job_id, status)
        for (job_id, status) in failed_jobs.items()
    ])
  if passed_jobs:
    msg += '#\nPASS:\n'
    msg += ''.join(['- %s/%s\n' % (log_url, job_id) for job_id in passed_jobs])
  if ui_links:
    msg += '\nArtifacts:\n' + ''.join('- %s\n' % link for link in ui_links)
  msg += 'CI page for this CL:\n'
  msg += '- https://ci.perfetto.dev/#!/cls/%s\n' % cl_and_ps.split('-')[0]
  body = {'labels': {}, 'message': msg}
  if not cancelled:
    body['labels']['Code-Review'] = cl_vote
  logging.info('Posting results for CL %s', cl_and_ps)
  url = 'https://%s/a/changes/%s/revisions/%s/review' % (
      GERRIT_HOST, cl_obj['change_id'], cl_obj['revision_id'])
  await req_async('POST', url, body=body, gerrit=True)
  await req_async('PUT', '%s/cls/%s/voted.json' % (DB, cl_and_ps), body=True)


async def queue_postsubmit_jobs(branch: str, revision: str = None):
  '''Creates the jobs entries in the DB for the given branch or revision

  Can be called in two modes:
    1. ?branch=main: Will retrieve the SHA1 of main and call the one below.
    2. ?branch=main&rev=deadbeef1234: queues jobs for the given revision.
  '''
  prj = urllib.parse.quote(GERRIT_PROJECT, '')
  assert (branch)

  if not revision:
    # Get the commit SHA1 of the head of the branch.
    url = 'https://%s/a/projects/%s/branches/%s' % (GERRIT_HOST, prj, branch)
    revision = (await req_async('GET', url, gerrit=True))['revision']
    assert (revision)
    # If the latest entry matches the revision, quit without queueing another
    # set of jobs for the same CL. This is an optimization to avoid wasting
    # compute over the weekend to rebuild the same revision every hour.
    filt = 'orderBy="$key"&limitToLast=1'
    cl_objs = await req_async('GET', '%s/branches.json?%s' % (DB, filt)) or {}
    if cl_objs and next(iter(cl_objs.values())).get('rev') == revision:
      logging.debug('Skipping postsubmits for %s: already run', revision)
      return
    await queue_postsubmit_jobs(branch=branch, revision=revision)
    return

  # Get the committer datetime for the given revision.
  url = 'https://%s/a/projects/%s/commits/%s' % (GERRIT_HOST, prj, revision)
  commit_info = await req_async('GET', url, gerrit=True)
  time_committed = commit_info['committer']['date'].split('.')[0]
  time_committed = datetime.strptime(time_committed, '%Y-%m-%d %H:%M:%S')

  # Enqueue jobs.
  src = 'branches/%s-%s' % (branch, time_committed.strftime('%Y%m%d%H%M%S'))
  now = datetime.utcnow()
  patch_obj = {
      src: {
          'rev': revision,
          'subject': commit_info['subject'][:100],
          'author': commit_info['author'].get('email', 'N/A'),
          'time_committed': utc_now_iso(time_committed),
          'time_queued': utc_now_iso(),
          'jobs': {},
      }
  }
  ref = 'refs/heads/' + branch
  append_jobs(patch_obj, src, ref, now)
  await req_async('PATCH', DB + '.json', body=patch_obj)


async def delete_expired_logs(ttl_days=LOGS_TTL_DAYS):
  url = '%s/logs.json?limitToFirst=1000&shallow=true' % (DB)
  logs = await req_async('GET', url) or {}
  tasks = []
  logging.debug('delete_expired_logs: got %d keys', len(logs.keys()))
  for job_id in logs.keys():
    age_days = (datetime.now() - datetime.strptime(job_id[:8], '%Y%m%d')).days
    if age_days > ttl_days:
      logging.debug('Delete log %s', job_id)
      tasks.append(defer(delete_job_logs(job_id=job_id)))
  await asyncio.gather(*tasks)


async def delete_stale_jobs():
  '''Deletes jobs that are left in the running queue for too long

  This is usually due to a crash in the VM that handles them.
  '''
  running_jobs = await req_async('GET', '%s/jobs_running.json?shallow=true' %
                                 (DB)) or {}
  tasks = []
  for job_id in running_jobs.keys():
    job = await req_async('GET', '%s/jobs/%s.json' % (DB, job_id))
    time_started = parse_iso_time(job.get('time_started', utc_now_iso()))
    age = (datetime.now() - time_started).total_seconds()
    if age > JOB_TIMEOUT_SEC * 2:
      tasks.append(defer(cancel_job(job_id=job_id)))
  await asyncio.gather(*tasks)


async def delete_stale_workers():
  '''Deletes workers that have been inactive for too long

  This is usually due to a crash in the VM that handles them.
  '''
  workers = await req_async('GET', '%s/workers.json' % (DB)) or {}
  patch_obj = {}
  for worker_id, worker in workers.items():
    last_update = parse_iso_time(worker.get('last_update', utc_now_iso()))
    age = (datetime.now() - last_update).total_seconds()
    if age > 60 * 60 * 12:
      patch_obj['workers/' + worker_id] = {}  # DELETE
  if len(patch_obj) == 0:
    return
  logging.info('Purging %d inactive workers', len(patch_obj))
  await req_async('PATCH', DB + '.json', body=patch_obj)


async def cancel_job(job_id: str):
  '''Cancels a job if not completed or failed.

  This function is racy: workers can complete the queued jobs while we mark them
  as cancelled. The result of such race is still acceptable.'''
  status = await req_async('GET', '%s/jobs/%s/status.json' % (DB, job_id))
  patch_obj = {
      'jobs_running/%s' % job_id: {},  # = DELETE,
      'jobs_queued/%s' % job_id: {},  # = DELETE,
  }
  if status in ('QUEUED', 'STARTED'):
    patch_obj['jobs/%s/status' % job_id] = 'CANCELLED'
    patch_obj['jobs/%s/time_ended' % job_id] = utc_now_iso()
  await req_async('PATCH', DB + '.json', body=patch_obj)


async def requeue_job(job_id: str):
  '''Re-queues a job that was previously interrupted due to a VM shutdown.'''
  logging.info('Requeuing interrupted job %s', job_id)
  patch_obj = {
      'jobs_running/%s' % job_id: {},  # = DELETE,
      'jobs_queued/%s' % job_id: 0,
      'jobs/%s/status' % job_id: 'QUEUED',
      'jobs/%s/time_queued' % job_id: utc_now_iso(),
      'jobs/%s/time_started' % job_id: {},  # = DELETE
      'jobs/%s/time_ended' % job_id: {},  # = DELETE
      'jobs/%s/worker' % job_id: {},  # = DELETE
  }
  await req_async('PATCH', DB + '.json', body=patch_obj)


async def delete_job_logs(job_id: str):
  await req_async('DELETE',
                  '%s/logs/%s.json?writeSizeLimit=unlimited' % (DB, job_id))


async def update_cl_metrics(src: str):
  cl_obj = await req_async('GET', '%s/%s.json' % (DB, src))
  t_queued = parse_iso_time(cl_obj['time_queued'])
  t_ended = parse_iso_time(cl_obj['time_ended'])
  await write_metrics({
      'ci_cl_completion_time': {
          'l': {},
          'v': int((t_ended - t_queued).total_seconds())
      }
  })


async def update_job_metrics(job_id: str):
  job = await req_async('GET', '%s/jobs/%s.json' % (DB, job_id))
  metrics = {}
  if 'time_queued' in job and 'time_started' in job:
    t_queued = parse_iso_time(job['time_queued'])
    t_started = parse_iso_time(job['time_started'])
    metrics['ci_job_queue_time'] = {
        'l': {
            'job_type': job['type']
        },
        'v': int((t_started - t_queued).total_seconds())
    }
  if 'time_ended' in job and 'time_started' in job:
    t_started = parse_iso_time(job['time_started'])
    t_ended = parse_iso_time(job['time_ended'])
    metrics['ci_job_run_time'] = {
        'l': {
            'job_type': job['type']
        },
        'v': int((t_ended - t_started).total_seconds())
    }
  if metrics:
    await write_metrics(metrics)


async def update_queue_metrics():
  # Update the stackdriver metric that will drive the autoscaler.
  queued = await req_async('GET', DB + '/jobs_queued.json?shallow=true') or {}
  running = await req_async('GET', DB + '/jobs_running.json?shallow=true') or {}
  logging.debug('ci_job_queue_len: %d + %d', len(queued), len(running))
  await write_metrics({'ci_job_queue_len': {'v': len(queued) + len(running)}})


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
