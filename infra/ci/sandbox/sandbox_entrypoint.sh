#!/bin/bash
# Copyright (C) 2025 The Android Open Source Project
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

set -eu -o pipefail

# Move to tmpfs, as GitHub Action runner checks out the repo under
# a _work subdirectory
cp -a github-action-runner /tmp
cd /tmp/github-action-runner/

GITHUB_TOKEN=$(cat "$GITHUB_TOKEN_PATH")

./config.sh --unattended --ephemeral --replace \
    --url "https://github.com/$GITHUB_REPO" \
    --token "$GITHUB_TOKEN"

# Setup Google Cloud config.
# Token is generated in 'infra/ci/worker/sandbox_runner.py'.
gcloud config set auth/access_token_file "$SVC_TOKEN_PATH"
# Suppress warning about "parallel composite upload" when doing 'gcloud storage cp ...' .
gcloud config set storage/parallel_composite_upload_enabled True

trap cleanup SIGTERM

cleanup() {
  echo "Received SIGTERM. Removing Action runner..."
  kill $pid
  wait $pid
  ./config.sh remove --token "$GITHUB_TOKEN"
}

# Run the GitHub Action Runner
GITHUB_TOKEN="" ./run.sh &
pid=$!

wait $pid
