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

# Save CI time by skipping runs on {UI,docs,infra}-only changes
if [[ $UI_DOCS_INFRA_ONLY_CHANGE == 1 ]]; then
echo "Detected non-code change, probably a UI-only change."
echo "skipping build + test runs"
exit 0
fi

BAZEL_DISK_CACHE_FOLDER="/ci/cache/bazel-disk-cache-$(hostname)"
readonly BAZEL_DISK_CACHE_FOLDER
# Cleanup the cache if any of the two conditions are true.
BAZEL_DISK_CACHE_GC_OPTIONS="--experimental_disk_cache_gc_max_age=7d --experimental_disk_cache_gc_max_size=10G"
# We don't run a bazel daemon in background, so we do a GC during the build,
# that's why we specify _idle_delay=0.
BAZEL_DISK_CACHE_GC_OPTIONS+=" --experimental_disk_cache_gc_idle_delay=0"
readonly BAZEL_DISK_CACHE_GC_OPTIONS

BAZEL_DISK_CACHE_FLAGS="--disk_cache=${BAZEL_DISK_CACHE_FOLDER} ${BAZEL_DISK_CACHE_GC_OPTIONS}"
readonly BAZEL_DISK_CACHE_FLAGS

# shellcheck disable=SC2086
tools/bazel build //:all ${BAZEL_DISK_CACHE_FLAGS} --verbose_failures
# shellcheck disable=SC2086
tools/bazel build //python:all ${BAZEL_DISK_CACHE_FLAGS} --verbose_failures

# Smoke test that processes run without crashing.
./bazel-bin/traced &
./bazel-bin/traced_probes &
sleep 5
TRACE=/ci/artifacts/bazel.trace
./bazel-bin/perfetto -c :test -o $TRACE
kill $(jobs -p)
./bazel-bin/trace_processor_shell -q <(echo 'select count(1) from sched') $TRACE

# Check the amalgamated build here to avoid slowing down all the Linux bots.
echo -e "\n\n***** Testing amalgamated build *****\n"
tools/test_gen_amalgamated.py

# Print the size of the bazel cache to make sure it won't grow infinitely.
du -sh "${BAZEL_DISK_CACHE_FOLDER}"
