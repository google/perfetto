#!/bin/bash
# Copyright (C) 2018 The Android Open Source Project
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

set -eux

SCRIPT_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"
ROOT_DIR="$(realpath ${SCRIPT_DIR}/../..)"

cd ${ROOT_DIR}

# Check that the expected environment variables are present (due to set -u).
echo PERFETTO_TEST_GN_ARGS: ${PERFETTO_TEST_GN_ARGS}

OUT_PATH="out/dist"

tools/install-build-deps --no-android

pip install --quiet --user protobuf

if [[ -e buildtools/clang/bin/llvm-symbolizer ]]; then
  export ASAN_SYMBOLIZER_PATH="buildtools/clang/bin/llvm-symbolizer"
  export MSAN_SYMBOLIZER_PATH="buildtools/clang/bin/llvm-symbolizer"
fi

tools/gn gen ${OUT_PATH} --args="${PERFETTO_TEST_GN_ARGS}" --check
tools/ninja -C ${OUT_PATH}

# Run the tests
${OUT_PATH}/perfetto_unittests
${OUT_PATH}/perfetto_integrationtests

BENCHMARK_FUNCTIONAL_TEST_ONLY=true ${OUT_PATH}/perfetto_benchmarks
tools/diff_test_trace_processor.py \
  --test-type=queries ${OUT_PATH}/trace_processor_shell
tools/diff_test_trace_processor.py \
  --test-type=metrics ${OUT_PATH}/trace_processor_shell
