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


: "${PERFETTO_CACHE_DIR:?PERFETTO_CACHE_DIR is not set}"

CCACHE_DIR=$PERFETTO_CACHE_DIR/ccache
mkdir -p $CCACHE_DIR
DEPS_SHA=$(shasum "tools/install-build-deps" | awk '{print $1}')
echo "DEPS_SHA=$DEPS_SHA"
echo "CCACHE_BASEDIR=$(pwd)"
echo "CCACHE_DIR=$CCACHE_DIR"
echo "CCACHE_MAXSIZE=8G"
echo "CCACHE_SLOPPINESS=include_file_ctime,include_file_mtime"
echo "CCACHE_COMPRESS=1"
# Default compress level for version 3.7 that we use on CI.
echo "CCACHE_COMPRESSLEVEL=6"
echo "CCACHE_COMPILERCHECK=string:$DEPS_SHA"
echo "CCACHE_UMASK=000"
echo "CCACHE_DEPEND=1"
