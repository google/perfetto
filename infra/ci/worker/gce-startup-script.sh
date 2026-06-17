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

# Create a scratch /tmp for checkout + build + cache. We don't care about data
# persistency as the only things that are persisted (artifacts) are uploaded to
# GCS.
shopt -s nullglob
SSDS=(/dev/nvme0n*)
mdadm --create /dev/md0 --level=0 --force --run \
      --raid-devices=${#SSDS[@]} "${SSDS[@]}"
# ext4 with no journal is the fastest config; -E nodiscard avoids TRIM overhead.
mkfs.ext4 -F -O ^has_journal -E nodiscard /dev/md0
mount -o noatime,lazytime,nodiscard /dev/md0 /tmp
chmod 1777 /tmp

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
