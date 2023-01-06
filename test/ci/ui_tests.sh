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

INSTALL_BUILD_DEPS_ARGS="--ui"
source $(dirname ${BASH_SOURCE[0]})/common.sh

infra/perfetto.dev/build

ui/build --out ${OUT_PATH}

cp -a ${OUT_PATH}/ui/dist/ /ci/artifacts/ui

ui/run-unittests --out ${OUT_PATH} --no-build

set +e
ui/run-integrationtests --out ${OUT_PATH} --no-build
RES=$?

# Copy the output of screenshots diff testing.
if [ -d ${OUT_PATH}/ui-test-artifacts ]; then
  cp -a ${OUT_PATH}/ui-test-artifacts /ci/artifacts/ui-test-artifacts
  exit $RES
fi
