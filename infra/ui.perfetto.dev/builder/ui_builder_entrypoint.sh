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

# ENTRYPOINT for the perfetto-ui-builder Docker image, invoked from
# infra/ui.perfetto.dev/cloudbuild*.yaml.
#
# $1 is the branch name (or 'autopush' literal from cloudbuild.yaml).
# Cloud Build has already placed us on the SHA that fired the trigger,
# so we build from HEAD without any explicit checkout.
#
# Branches autopush/canary/stable map 1:1 to channels of the same name.
# Any other branch (i.e. a release branch) maps to the 'release' mode,
# which uploads /v<version>/ only and does NOT touch the shared root
# index.html or service_worker.
#
# See go/perfetto-ui-autopush for end-to-end docs.

set -exu

cd /workspace/

git config --global init.defaultBranch main
git fetch --unshallow
git rev-parse HEAD

case "$1" in
  autopush|canary|stable) CHANNEL="$1" ;;
  *)                      CHANNEL="release" ;;
esac
python3 -u ui/release/build_channel.py --channel="$CHANNEL" --upload
