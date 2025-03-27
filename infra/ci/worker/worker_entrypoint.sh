#!/bin/bash
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

set -eux -o pipefail

# This is the initial script that runs as root in the one worker docker image
# that runs as first thing in each GCE vm.
# The docker container that runs this script is privileged. This container is
# not for isolation (the sandbox is), it's just for ease of deployment.


# The vars below are defined in config.py and set at VM creation time in the
# Makefile. It's essentially the way we pass "cmdline arguments" to a GCE vm.

ATTRS='http://metadata.google.internal/computeMetadata/v1/instance/attributes'
URL="$ATTRS/num-workers"
NUM_WORKERS=$(curl --silent --fail -H'Metadata-Flavor:Google' $URL || echo 1)

URL="$ATTRS/sandbox-img"
SANDBOX_IMG=$(curl --silent --fail -H'Metadata-Flavor:Google' $URL)


# Pull the latest images from the registry.
docker pull $SANDBOX_IMG

# Create the restricted bridge for the sandbox container.
# Prevent access to the metadata server and impersonation of service accounts.
docker network rm sandbox 2>/dev/null || true  # Handles the reboot case.
docker network create sandbox -o com.docker.network.bridge.name=sandbox
iptables -I DOCKER-USER -i sandbox -d 169.254.0.0/16 -j REJECT

export PYTHONUNBUFFERED=1

# The current dir is set by the Dockerfile WORKDIR.
pwd

cat << EOF > supervisord.conf
[supervisord]
nodaemon=true
loglevel=warn
user=root
logfile=/dev/stdout
logfile_maxbytes=0


[program:sandbox_runner]
process_name=%(program_name)s_%(process_num)d
numprocs=${NUM_WORKERS}
command=python3 $(pwd)/sandbox_runner.py
environment=SANDBOX_ID="%(process_num)s"
autostart=true
autorestart=true
stdout_logfile=/dev/fd/1
stderr_logfile=/dev/fd/2
stdout_logfile_maxbytes=0
stderr_logfile_maxbytes=0
stopasgroup=true
killasgroup=true
EOF

exec supervisord -c supervisord.conf
