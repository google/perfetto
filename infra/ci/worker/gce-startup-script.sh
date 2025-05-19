#!/bin/bash
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

set -eux -o pipefail

# worker-img is set at GCE VM creation time in the Makefile.

ATTRS='http://metadata.google.internal/computeMetadata/v1/instance/attributes'
URL="$ATTRS/worker-img"
WORKER_IMG=$(curl --silent --fail -H'Metadata-Flavor:Google' $URL)

# We use for checkout + build + cache. The only things that are persisted
# (artifacts) are uploaded to GCS. We use swapfs so we can exceed physical ram
# when using tmpfs. It's still faster than operating on a real filesystem as in
# most cases we have enough ram to not need swap.
shopt -s nullglob
for SSD in /dev/nvme0n*; do
mkswap $SSD
swapon -p -1 $SSD
done

# Enlarge the /tmp size to use swap and also allow exec, as some tests need it.
mount -o remount,mode=777,size=95%,exec /tmp

# Disable Addressa space ranomization. This is needed for Tsan tests to work in
# the sandbox. Newer kernel versions seem to block the
# personality(ADDR_NO_RANDOMIZE) call within the sandbox and would require the
# sandbox to be --privileged, which defeats the point of having a sandbox.
sudo sysctl -w kernel.randomize_va_space=0

docker run -d \
  --name worker \
  --privileged \
  --net=host \
  --log-driver gcplogs \
  --rm \
  -v /tmp:/tmp \
  -v /var/run/docker.sock:/var/run/docker.sock \
  $WORKER_IMG


cat<<'EOF'>/tmp/shutdown_when_idle.sh
# This script initiates a VM shutdown when all the sandboxes have been idle
# for long time. After b/417658206, we use the autoscaler only to scale up based
# on the CI queue length.
# Scale down is triggered by idle detection, to avoid accidentally terminating
# ongoing CI jobs.
# sandbox_runner.py updates the timestamp of /run/perfetto_ci_lastrun before
# each iteration (hence at the end of each).
# Note that technically we cannot tell the when a sandbox is "idle" because we
# cannot tell the difference between "the sandbox is waiting for the next job"
# or "the sandbox picked up a job and is running it (it will terminate after)".
# But we can rely on the fact that GitHub Action Runners have a 60 min timeout.
# So the 90 minutes below gives us a 30 min grace period of actual idleness in
# the worst case.

set -eu -o pipefail

FILE=/tmp/perfetto_ci_lastrun

echo "Starting idle shutdown monitor..."

while true; do
  if [ -f "$FILE" ]; then
    if [ "$(find "$FILE" -mmin +90)" ]; then
      echo "[$(date)] All sandboxes idle. Shutting down..."
      shutdown -h now
    fi
  else
    echo "[$(date)] File not found: $FILE"
  fi
  sleep 60
done
EOF

chmod 755 /tmp/shutdown_when_idle.sh

setsid nohup /tmp/shutdown_when_idle.sh > /tmp/shutdown_when_idle.log 2>&1 &
