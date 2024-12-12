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

source $(dirname ${BASH_SOURCE[0]})/common.sh

export CI=1

infra/perfetto.dev/build

ui/build --out ${OUT_PATH}

cp -a ${OUT_PATH}/ui/dist/ /ci/artifacts/ui

ui/run-unittests --out ${OUT_PATH} --no-build

set +e

# Install chrome
(
  mkdir /ci/ramdisk/chrome
  cd /ci/ramdisk/chrome
  CHROME_VERSION=128.0.6613.137
  curl -Ls -o chrome.deb https://dl.google.com/linux/chrome/deb/pool/main/g/google-chrome-stable/google-chrome-stable_${CHROME_VERSION}-1_amd64.deb
  dpkg-deb -x chrome.deb  .
)
ui/run-integrationtests --out ${OUT_PATH} --no-build
RES=$?

set +x

# Copy the output of screenshots diff testing.
if [ -d ${OUT_PATH}/ui-test-artifacts ]; then
  cp -a ${OUT_PATH}/ui-test-artifacts /ci/artifacts/ui-test-artifacts
  echo "UI integration test report with screnshots:"
  echo "https://storage.googleapis.com/perfetto-ci-artifacts/$PERFETTO_TEST_JOB/ui-test-artifacts/index.html"
  echo ""
  echo "To download locally the changed screenshots run:"
  echo "tools/download_changed_screenshots.py $PERFETTO_TEST_JOB"
  echo ""
  echo "Perfetto UI build for this CL"
  echo "https://storage.googleapis.com/perfetto-ci-artifacts/$PERFETTO_TEST_JOB/ui/index.html"
  exit $RES
fi
