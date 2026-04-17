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
# $1 must be the channel name to build: autopush | canary | stable.
# Cloud Build has already placed us on the SHA that fired the trigger
# (main HEAD for autopush, canary HEAD for canary, stable HEAD for
# stable), so we build from HEAD without any explicit checkout.
#
# See go/perfetto-ui-autopush for end-to-end docs.

set -exu

cd /workspace/

git config --global init.defaultBranch main
git fetch --unshallow
git rev-parse HEAD

CHANNEL="$1"
python3 -u ui/release/build_channel.py --channel="$CHANNEL" --upload
