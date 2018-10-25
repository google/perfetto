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

set -eux

SCRIPT_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"
ROOT_DIR="$(realpath ${SCRIPT_DIR}/../..)"

cd ${ROOT_DIR}

# Check that the expected environment variables are present (due to set -u).
echo PERFETTO_TEST_GN_ARGS: ${PERFETTO_TEST_GN_ARGS}
echo PERFETTO_TEST_ENTRYPT: ${PERFETTO_TEST_ENTRYPT}

# TODO(rsavitski): figure out how to copy files into the container without
# requiring o= permissions on the ${ROOT_DIR} subtree.
# TODO(rsavitski): switch from :experimental to :latest image
# Note: SYS_PTRACE capability is added for [at least] the leak sanitizer.
sudo docker run --rm -t \
  --cap-add SYS_PTRACE \
  -e PERFETTO_TEST_GN_ARGS="${PERFETTO_TEST_GN_ARGS}" \
  -v ${ROOT_DIR}:/perfetto:ro \
  asia.gcr.io/perfetto-ci/perfetto-ci:experimental \
  /bin/bash \
  "-c" \
  "cp -r /perfetto /home/perfetto/src && \
  cd /home/perfetto/src && \
  ${PERFETTO_TEST_ENTRYPT}"
