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
# pulled the GitHub repo in shallow mode.

# The cd is really a safeguard against people running this script on their
# workstation and hitting the rm -rf.
cd /workspace/
git rev-parse HEAD

# Install only NodeJS, gn and ninja no need to install the other toolchains.
tools/install-build-deps --ui --filter=nodejs --filter=gn --filter=ninja --filter=pnpm

# The deploy script takes care of building by invoking ./build internally.
infra/perfetto.dev/deploy
