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

from config import PROJECT

STACKDRIVER_METRICS = {
    'ci_job_queue_len': {
        'name': 'ci_job_queue_len',
        'displayName': 'ci_job_queue_len',
        'description': 'Length of the CI jobs queue',
        'type': 'custom.googleapis.com/%s/ci_job_queue_len' % PROJECT,
        'metricKind': 'GAUGE',
        'valueType': 'INT64',
        'metadata': {
            'samplePeriod': {
                'seconds': 1
            }
        },
        'labels': []
    },
    'ci_job_queue_time': {
        'name': 'ci_job_queue_time',
        'displayName': 'ci_job_queue_time',
        'description': 'Queueing time of CI jobs, before they start running',
        'type': 'custom.googleapis.com/%s/ci_job_queue_time' % PROJECT,
        'metricKind': 'GAUGE',
        'valueType': 'INT64',
        'unit': 's',
        'metadata': {
            'samplePeriod': {
                'seconds': 1
            }
        },
        'labels': [{
            'key': 'job_type',
            'valueType': 'STRING'
        }]
    },
    'ci_job_run_time': {
        'name': 'ci_job_run_time',
        'displayName': 'ci_job_run_time',
        'description': 'Running time of CI jobs',
        'type': 'custom.googleapis.com/%s/ci_job_run_time' % PROJECT,
        'metricKind': 'GAUGE',
        'valueType': 'INT64',
        'unit': 's',
        'metadata': {
            'samplePeriod': {
                'seconds': 1
            }
        },
        'labels': [{
            'key': 'job_type',
            'valueType': 'STRING'
        }]
    },
    'ci_cl_completion_time': {
        'name': 'ci_cl_completion_time',
        'displayName': 'ci_cl_completion_time',
        'description': 'Time it takes for all jobs of a CL to complete',
        'type': 'custom.googleapis.com/%s/ci_cl_completion_time' % PROJECT,
        'metricKind': 'GAUGE',
        'valueType': 'INT64',
        'unit': 's',
        'metadata': {
            'samplePeriod': {
                'seconds': 1
            }
        },
        'labels': []
    },
}
