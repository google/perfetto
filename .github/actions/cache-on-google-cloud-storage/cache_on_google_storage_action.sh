#!/bin/bash
set -eu -o pipefail

function do_validate_input() {
  if [[ "$INPUT_ACTION" != "save" && "$INPUT_ACTION" != "restore" ]]; then
    echo "Invalid \$INPUT_ACTION: should be 'save' or 'restore', got: '$INPUT_ACTION'."
    exit 1
  fi

  # Check if 'inputs.directory' exists.
  if [[ ! -d "$INPUT_DIRECTORY" ]]; then
    echo "Invalid input 'directory': the directory '$INPUT_DIRECTORY' does not exist."
    exit 1
  fi

  # Check if 'inputs.cache_key' exists and matches the regex.
  if [[ ! "$INPUT_CACHE_KEY" =~ $ALLOWED_CACHE_KEY_REGEX ]]; then
    echo "Invalid input 'cache_key': '$INPUT_CACHE_KEY', allowed regex: '$ALLOWED_CACHE_KEY_REGEX'"
    exit 1
  fi

  # Check if 'inputs.exclude_files' is provided and if the file actually exists
  if [[ -n "$INPUT_EXCLUDE_FILES_PATH" ]]; then
    if [[ ! -f "$INPUT_EXCLUDE_FILES_PATH" ]]; then
      echo "Invalid input 'exclude_files': file '$INPUT_EXCLUDE_FILES_PATH' was specified but does not exist."
      exit 1
    fi
  fi
}

function do_restore_cache() {
  CACHED_TAR_PATH=$(mktemp /tmp/restored-cache-XXXXXX.tar)
  trap 'rm -f "$CACHED_TAR_PATH"' EXIT

  cp_start_time=$SECONDS
  cp_output=""
  if cp_output=$(gcloud storage cp "$GCS_CACHE_PATH" "$CACHED_TAR_PATH" 2>&1); then
    cp_duration=$((SECONDS - cp_start_time))
    echo "$cp_output"
    echo "The 'gcloud storage cp' took $cp_duration seconds to complete."

    declare -a tar_args
    # '--keep-old-files' flag causes 'tar' to fail instead of overwriting files.
    tar_args=("--keep-old-files" "--preserve-permissions" "--extract")
    # '--exclude-from' is used to not overwrite files that already exists in the output directory.
    if [[ -n "$INPUT_EXCLUDE_FILES_PATH" ]]; then
      tar_args+=("--exclude-from=$INPUT_EXCLUDE_FILES_PATH")
    fi
    tar_args+=("--file=$CACHED_TAR_PATH" "--directory=$INPUT_DIRECTORY")

    tar "${tar_args[@]}"

    rm "$CACHED_TAR_PATH"

    echo "cache_hit=true" >> "$GITHUB_OUTPUT"
    echo "Cache restored, OK"
  elif [[ "$cp_output" =~ "The following URLs matched no objects or files" ]]; then
    echo "cache_hit=false" >> "$GITHUB_OUTPUT"
    echo "Cache not found, OK"
  else
    echo "Can't download cache: $cp_output"
    echo "cache_hit=false" >> "$GITHUB_OUTPUT"
    exit 1
  fi
}

function do_save_cache() {
  TAR_PATH=$(mktemp /tmp/cache-XXXXXX.tar)
  trap 'rm -f "$TAR_PATH"' EXIT

  declare -a tar_args
  # '--sort=name' is used to make archive more reproducible.
  tar_args=("--create" "--sort=name" "--preserve-permissions")
  # '--exclude-from' is used to not cache files or directories that we don't want to.
  if [[ -n "$INPUT_EXCLUDE_FILES_PATH" ]]; then
    tar_args+=("--exclude-from=$INPUT_EXCLUDE_FILES_PATH")
  fi
  tar_args+=("--file=$TAR_PATH" "--directory=$INPUT_DIRECTORY" ".")

  tar "${tar_args[@]}"

  cp_start_time=$SECONDS
  gcloud storage cp "$TAR_PATH" "$GCS_CACHE_PATH"
  cp_duration=$((SECONDS - cp_start_time))
  echo "The 'gcloud storage cp' took $cp_duration seconds to complete."

  rm "$TAR_PATH"
}

function main() {
  do_validate_input

  VALIDATED_KEY=$(echo "$INPUT_CACHE_KEY" | tr ' -' '_')
  readonly GCS_CACHE_PATH="$GCS_BUCKET/cache/$VALIDATED_KEY/archive.tar"

  if [[ "$INPUT_ACTION" == "save" ]]; then
    do_save_cache
  elif [[ "$INPUT_ACTION" == "restore" ]]; then
    do_restore_cache
  else
    echo "Unsupported \$INPUT_ACTION: '$INPUT_ACTION'."
    exit 1
  fi
}

# TODO(ktimofeev): set as an environment variable in 'infra/ci/worker/sandbox_runner.py'
readonly GCS_BUCKET="gs://perfetto-ci-artifacts"
readonly ALLOWED_CACHE_KEY_REGEX="[a-zA-Z0-9._ -]+"

# This script is called from 'save/action.yml' and 'restore/action.yml' GitHub actions.
# It expect the following variable being set by the caller:
# `$INPUT_ACTION`, `$INPUT_DIRECTORY`, `$INPUT_CACHE_KEY`, `$INPUT_EXCLUDE_FILES_PATH`.
# When restoring cache, the cache output directory is not created, it should be already created by the caller.
main "$@"
