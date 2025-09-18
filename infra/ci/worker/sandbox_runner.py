#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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
''' Runs one GitHub (Ephemeral) Action Runner and quits at the end.

The Action Runner is executed in its own sandboxed docker container, which has
no access to the metadata server.
It also handles graceful container termination upon shutdown.
'''

import logging
import os
import signal
import socket
import subprocess
import time
import sys

from config import SANDBOX_IMG, GITHUB_REPO, SANDBOX_SVC_ACCOUNT
from common_utils import get_github_registration_token
from pathlib import Path

CUR_DIR = os.path.dirname(__file__)

# The container name will be GCE_HostName-N.
SANDBOX_ID = os.environ.get('SANDBOX_ID', '')
SANDBOX_NAME = '%s-%s' % (socket.gethostname(), SANDBOX_ID)


def sig_handler(_, __):
  logging.warning('Interrupted by signal, exiting worker')
  subprocess.call(['docker', 'stop', SANDBOX_NAME])
  sys.exit(0)


def create_sandbox_token(token_path):
  # Impersonate the sandbox service account. This creates a short-lived 1h token
  # for a downgraded service account that we pass to the sandbox. The sandbox
  # service account is allowed only storage object creation for untrusted CI
  # artifacts.
  sandbox_svc_token = subprocess.check_output([
      'gcloud',
      'auth',
      'application-default',
      'print-access-token',
      '--impersonate-service-account=%s' % SANDBOX_SVC_ACCOUNT,
      # TODO(primiano): uncomment below after b/406184705 is fixed.
      # '--lifetime=21600',
  ]).decode().strip()
  with open(token_path + '.tmp', 'w') as f:
    f.write(sandbox_svc_token)
  os.rename(token_path + '.tmp', token_path)


def main():
  logging.basicConfig(
      format='%(levelname)-8s %(asctime)s ' + SANDBOX_NAME + ' %(message)s',
      level=logging.DEBUG if os.getenv('VERBOSE') else logging.INFO,
      datefmt=r'%Y-%m-%d %H:%M:%S')
  logging.info('sandbox_runner started')

  signal.signal(signal.SIGTERM, sig_handler)
  signal.signal(signal.SIGINT, sig_handler)

  # Update the mtime of perfetto_ci_lastrun. This is used to shutdown the
  # GCE vm when idle for too long.
  Path('/tmp/perfetto_ci_lastrun').touch()

  # Remove stale sandbox from previous runs, if any.
  subprocess.call(['docker', 'rm', '-f', SANDBOX_NAME],
                  stderr=subprocess.DEVNULL)

  # Run the nested docker container that will execute the ephemeral GitHub
  # action runner in the sandbox image.
  cmd = [
      'docker', 'run', '--rm', '--name', SANDBOX_NAME, '--hostname',
      SANDBOX_NAME, '--stop-timeout', '60', '--cap-add', 'SYS_PTRACE',
      '--network', 'sandbox', '--dns', '8.8.8.8', '--log-driver', 'gcplogs'
  ]
  # We use the tmpfs mount created by gce-startup-script.sh. The problem is that
  # Docker doesn't allow to both override the tmpfs-size and prevent the
  # "-o noexec".
  tmp_dir = '/tmp/' + SANDBOX_NAME
  subprocess.call(['rm', '-rf', tmp_dir])
  os.makedirs(tmp_dir, exist_ok=True)
  os.chmod(tmp_dir, 0o777)
  cmd += ['-v', '%s:/tmp' % tmp_dir]

  # Obtain the (short-lived) token to register the Github Action Runner and
  # pass it to the sandbox.
  github_token = get_github_registration_token()
  with open(os.path.join(tmp_dir, '.github_token'), 'w') as f:
    f.write(github_token)
  # Note: the path we see (/tmp/sandbox-N/.github_token) is different than what
  # the sandbox sees (which is just /tmp/.github_token due to the mount).
  cmd += ['--env', 'GITHUB_TOKEN_PATH=/tmp/.github_token']
  cmd += ['--env', 'GITHUB_REPO=%s' % GITHUB_REPO]

  svc_token_path = os.path.join(tmp_dir, '.svc_token')
  create_sandbox_token(svc_token_path)
  cmd += ['--env', 'SVC_TOKEN_PATH=/tmp/.svc_token']

  # The image name must be the last arg. Anything else would be interpreted as
  # a command to pass to the container.
  cmd += [SANDBOX_IMG]

  # This spawns the sandbox that runs one ephemeral GitHub Action job and
  # terminates when done.
  proc = subprocess.Popen(cmd)

  # Refresh the delegated service token every 50 minutes as its TTL=1h. The
  # sandbox could be up for several hours if there are no jobs pending.
  last_refresh_time = time.time()
  REFRESH_SECS = 50 * 60
  while True:
    try:
      sys.exit(proc.wait(60))
    except subprocess.TimeoutExpired:
      if time.time() - last_refresh_time > REFRESH_SECS:
        create_sandbox_token(svc_token_path)
        last_refresh_time = time.time()


if __name__ == '__main__':
  main()
