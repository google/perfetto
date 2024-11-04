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

set -eux -o pipefail

cd $(dirname ${BASH_SOURCE[0]})/../..
OUT_PATH="out/dist"

export PYTHONUNBUFFERED=1

tools/install-build-deps $PERFETTO_INSTALL_BUILD_DEPS_ARGS

# Assumes Linux. Windows should use /win/clang instead.
if [[ -e buildtools/linux64/clang/bin/llvm-symbolizer ]]; then
  export ASAN_SYMBOLIZER_PATH="$(readlink -f buildtools/linux64/clang/bin/llvm-symbolizer)"
  export MSAN_SYMBOLIZER_PATH="$(readlink -f buildtools/linux64/clang/bin/llvm-symbolizer)"
fi

# Performs checks on generated protos and build files.
tools/gn gen out/tmp.protoc --args="is_debug=false cc_wrapper=\"ccache\""
tools/gen_all --check-only out/tmp.protoc
rm -rf out/tmp.protoc

# Performs checks on SQL files.
tools/check_sql_modules.py
tools/check_sql_metrics.py

if !(git diff --name-only HEAD^1 HEAD | egrep -qv '^(ui|docs|infra|test/data/ui-screenshots)/'); then
export UI_DOCS_INFRA_ONLY_CHANGE=1
else
export UI_DOCS_INFRA_ONLY_CHANGE=0
fi
