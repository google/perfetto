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

function set_github_output() {
    local key="$1"
    local value="$2"
    if [[ -n "${GITHUB_OUTPUT:-}" ]]; then
        echo "${key}=${value}" >> "$GITHUB_OUTPUT"
    fi
}

function do_restore_cache() {
  # Variable used in a trap, so not declared as local.
  CACHED_TAR_PATH=$(mktemp /tmp/restored-cache-XXXXXX.tar)
  trap 'rm -f "$CACHED_TAR_PATH"' EXIT

  local cp_start_time=$SECONDS
  local cp_output=""
  if cp_output=$(gcloud storage cp "$GCS_CACHE_PATH" "$CACHED_TAR_PATH" 2>&1); then
    cp_duration=$((SECONDS - cp_start_time))
    echo "$cp_output"
    echo "The 'gcloud storage cp' took $cp_duration seconds to complete."

    declare -a tar_args
    # '--keep-old-files' flag causes 'tar' to fail instead of overwriting files.
    tar_args=("--extract" "--preserve-permissions" "--keep-old-files")
    # '--exclude-from' is used to not overwrite files that already exists in the output directory.
    if [[ -n "$INPUT_EXCLUDE_FILES_PATH" ]]; then
      tar_args+=("--exclude-from=$INPUT_EXCLUDE_FILES_PATH")
    fi
    if [[ -d "$INPUT_DIRECTORY" ]]; then
      # Extract to the existing directory.
      tar_args+=("--directory=$INPUT_DIRECTORY")
    else
      # If there is no "$INPUT_DIRECTORY" tar creates it.
      tar_args+=("--one-top-level=$INPUT_DIRECTORY")
    fi
    tar_args+=("--file=$CACHED_TAR_PATH")

    tar "${tar_args[@]}"

    set_github_output "cache_hit" "true"
    echo "Cache restored, OK"
  elif [[ "$cp_output" =~ "The following URLs matched no objects or files" ]]; then
    set_github_output "cache_hit" "false"
    echo "Cache not found, OK"
  else
    set_github_output "cache_hit" "false"
    echo "Can't download cache: $cp_output"
    exit 1
  fi
}

function do_save_cache() {
  # Variable used in a trap, so not declared as local.
  TAR_PATH=$(mktemp /tmp/cache-XXXXXX.tar)
  trap 'rm -f "$TAR_PATH"' EXIT

  declare -a tar_args
  # '--sort=name' is used to make archive more reproducible.
  tar_args=("--create" "--preserve-permissions" "--sort=name")
  # '--exclude-from' is used to not cache files or directories that we don't want to.
  if [[ -n "$INPUT_EXCLUDE_FILES_PATH" ]]; then
    tar_args+=("--exclude-from=$INPUT_EXCLUDE_FILES_PATH")
  fi
  tar_args+=("--file=$TAR_PATH" "--directory=$INPUT_DIRECTORY" ".")

  tar "${tar_args[@]}"

  local cp_start_time=$SECONDS
  gcloud storage cp "$TAR_PATH" "$GCS_CACHE_PATH"
  cp_duration=$((SECONDS - cp_start_time))
  echo "The 'gcloud storage cp' took $cp_duration seconds to complete."
}

function main() {
  if [[ ! "$INPUT_CACHE_KEY" =~ $ALLOWED_CACHE_KEY_REGEX ]]; then
    echo "Invalid input 'cache_key': '$INPUT_CACHE_KEY', allowed regex: '$ALLOWED_CACHE_KEY_REGEX'"
    exit 1
  fi

  local FIXED_CACHE_KEY
  FIXED_CACHE_KEY=$(echo "$INPUT_CACHE_KEY" | tr ' -' '_')
  readonly GCS_CACHE_PATH="$GCS_BUCKET/cache/$FIXED_CACHE_KEY/archive.tar"

  if [[ "$INPUT_ACTION" == "save" ]]; then
    do_save_cache
  elif [[ "$INPUT_ACTION" == "restore" ]]; then
    do_restore_cache
  else
    echo "Unsupported \$INPUT_ACTION: should be 'save' or 'restore', got '$INPUT_ACTION'."
    exit 1
  fi
}

# TODO(ktimofeev): set as an environment variable in 'infra/ci/worker/sandbox_runner.py'
readonly GCS_BUCKET="gs://perfetto-ci-artifacts"
readonly ALLOWED_CACHE_KEY_REGEX="[a-zA-Z0-9._ -]+"

# This script is called from 'save/action.yml' and 'restore/action.yml' GitHub actions.
# It expect the following variable to be set by the caller:
# `$INPUT_ACTION`, `$INPUT_DIRECTORY`, `$INPUT_CACHE_KEY`, `$INPUT_EXCLUDE_FILES_PATH`.
# This scripts is started with 'set -eu', so it fails if any of the variables are not set.
# We don't explicitly check that input paths are valid; 'tar' will check them
# and report any errors.
# We don't compress the archive before uploading because it mainly contains
# binary data that doesn't compress well.
main "$@"
