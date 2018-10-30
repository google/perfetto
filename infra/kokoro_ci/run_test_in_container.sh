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


# Run PERFETTO_TEST_ENTRYPOINT inside the container with the following setup:
# Mount (readonly) the current source directory inside the container. Enter the
# container as root, make a mutable copy the source tree, change it to be owned
# by the user "perfetto" (pre-created inside the docker image), and then invoke
# the test script as that user. The copying is run as root to not require the
# source tree to have the read permissions for the "other" users.
#
# SYS_PTRACE capability is added for [at least] the leak sanitizer.
sudo docker run --rm -t \
  --user=root:root \
  --cap-add=SYS_PTRACE \
  -e PERFETTO_TEST_GN_ARGS="${PERFETTO_TEST_GN_ARGS}" \
  -v ${ROOT_DIR}:/perfetto:ro \
  asia.gcr.io/perfetto-ci/perfetto-ci:latest \
  /bin/bash \
  "-c" \
  "cp -r /perfetto /home/perfetto/src && \
  chown -hR perfetto:perfetto /home/perfetto/src && \
  su perfetto -c \" cd /home/perfetto/src && ${PERFETTO_TEST_ENTRYPT}\""

