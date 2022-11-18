#!/bin/bash
# Copyright (C) 2021 The Android Open Source Project
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

set -exu

# This script will be run in /workspace after the Cloud Build environment has
# pulled the GitHub repo in shallow mode. We want to build off the official
# AOSP repo, not the GitHub mirror though, hence why the clone below.
# GitHub is used only as a trigger. This is because Google Cloud Build doesn't
# support yet triggering from Gerrit.
# See go/perfetto-ui-autopush for more details.

# The cd is really a safeguard against people running this script on their
# workstation and hitting the rm -rf.
cd /workspace/
ls -A1 | xargs rm -rf
UPSTREAM="https://android.googlesource.com/platform/external/perfetto.git"
git clone --depth 1 $UPSTREAM upstream

cd upstream/
git rev-parse HEAD

# Install only NodeJS, gn and ninja no need to install the other toolchains.
tools/install-build-deps --ui --filter=nodejs --filter=gn --filter=ninja

# The deploy script takes care of building by invoking ./build internally.
infra/perfetto.dev/deploy
