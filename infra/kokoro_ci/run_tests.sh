#!/bin/bash
# Copyright (C) 2018 The Android Open Source Project
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

set -ex

SCRIPT_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"
ROOT_DIR="$(realpath ${SCRIPT_DIR}/../..)"

cd ${ROOT_DIR}

# Make space for docker image by symlinking the hardcoded /var/lib/docker path
# to a tmpfs mount. Cargo culted from other projects' scripts.
sudo -n /etc/init.d/docker stop
sudo -n mv /var/lib/docker /tmpfs/
sudo -n ln -s /tmpfs/docker /var/lib/docker
sudo -n /etc/init.d/docker start

# Run a prebaked build+test configuration in a container.
# TODO(rsavitski): pass configuration via environment variables.
docker run --rm -t --user=perfetto:perfetto \
  -v ${ROOT_DIR}:/perfetto:ro \
  asia.gcr.io/perfetto-ci/perfetto-ci:latest \
  /bin/bash run_tests.sh
