#!/usr/bin/env python3
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
'''Project-wide configuration

This file is either imported from other python scripts or executed to generate
makefile dumps of the variables. This is so all vars can live in one place.
'''

import os
import sys

# Gerrit config
GERRIT_HOST = 'android-review.googlesource.com'
GERRIT_PROJECT = 'platform/external/perfetto'
GERRIT_REVIEW_URL = ('https://android-review.googlesource.com/c/' +
                     GERRIT_PROJECT)
REPO_URL = 'https://android.googlesource.com/' + GERRIT_PROJECT
GERRIT_VOTING_ENABLED = True
LOGLEVEL = 'info'

# IDs for the Perfetto CI GitHub app.
GITHUB_REPO = 'google/perfetto'
GITHUB_APP_ID = 1184402
GITHUB_APP_INSTALLATION_ID = 62928975

# Cloud config (GCE = Google Compute Engine, GAE = Google App Engine)
PROJECT = 'perfetto-ci'

GAE_VERSION = 'prod'
DB_ROOT = 'https://%s.firebaseio.com' % PROJECT
DB = DB_ROOT + '/ci'
SANDBOX_IMG = 'us-docker.pkg.dev/%s/containers/gh-sandbox' % PROJECT
WORKER_IMG = 'us-docker.pkg.dev/%s/containers/gh-worker' % PROJECT
CI_SITE = 'https://ci.perfetto.dev'
GCS_ARTIFACTS = 'perfetto-ci-artifacts'

JOB_TIMEOUT_SEC = 45 * 60  # 45 min
CL_TIMEOUT_SEC = 60 * 60 * 3  # 3 hours
LOGS_TTL_DAYS = 15
TRUSTED_EMAILS = '^.*@google.com$'

GCE_REGIONS = 'us-west1'
GCE_VM_NAME = 'gh-worker'
GCE_VM_TYPE = 'c2d-standard-32'
GCE_TEMPLATE = 'gh-worker-template'
GCE_GROUP_NAME = 'gh'
MAX_VMS_PER_REGION = 8
NUM_WORKERS_PER_VM = 4
AUTOSCALER_MIN = 6  # TODO(primiano) put back to 0 once autoscaler is sorted

GCE_SCOPES = [
    'https://www.googleapis.com/auth/cloud-platform',
    'https://www.googleapis.com/auth/devstorage.read_write',
    'https://www.googleapis.com/auth/firebase.database',
    'https://www.googleapis.com/auth/logging.write',
    'https://www.googleapis.com/auth/monitoring.write',
    'https://www.googleapis.com/auth/trace.append',
    'https://www.googleapis.com/auth/userinfo.email',
]

SANDBOX_SVC_ACCOUNT = 'gce-ci-sandbox@perfetto-ci.iam.gserviceaccount.com'

# TODO(primiano) remove this after dismantling old ci.
JOB_CONFIGS = {}

if __name__ == '__main__':
  import os
  import json
  import re
  import sys
  vars = dict(kv for kv in locals().items() if re.match('^[A-Z0-9_]+$', kv[0]))

  if len(sys.argv) > 1 and sys.argv[1] == 'makefile':
    deps_path = os.path.join(os.path.dirname(__file__), '.deps')
    if not os.path.exists(deps_path):
      os.mkdir(deps_path)
    gen_file = os.path.join(deps_path, 'config.mk')

    try:
      literals = (int, long, basestring)
    except NameError:
      literals = (int, str)

    with open(gen_file, 'w') as f:
      for k, v in vars.items():
        if isinstance(v, literals):
          f.write('override %s=%s\n' % (k, v))
        elif isinstance(v, list):
          f.write('override %s=%s\n' % (k, ','.join(v)))

    print(gen_file)

  if len(sys.argv) > 1 and sys.argv[1] == 'js':
    jsn = json.dumps(vars, indent=2)
    print('// Auto-generated by %s, do not edit.\n' %
          os.path.basename(__file__))
    print('\'use strict\';\n')
    print('const cfg = JSON.parse(`%s`);\n' % jsn.replace(r'\"', r'\\\"'))
