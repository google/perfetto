#!/bin/bash
# Copyright (C) 2022 The Android Open Source Project
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

set -eu

# For perfetto maintainers. Pulls out demangling-related sources out of
# llvm-project and repackages them as a single tar archive. This works around
# the fact that gitiles, when generating an on-demand archive, uses a current
# timestamp, so the hash of the archive is different every time you fetch.

# Usage example:
#   sh tools/repackage_llvm_demangler.sh 3b4c59c156919902c785ce3cbae0eee2ee53064d
#
# Then upload the tar with "gsutil cp -n -a public-read ... gs://perfetto/...",
# and update install-build-deps.

GIT_REF=$1

WORK_DIR=$(mktemp -d)
pushd . && cd "${WORK_DIR}"

CC_DIR="llvm-project/llvm/lib/Demangle"
H_DIR="llvm-project/llvm/include/llvm/Demangle"

CC_TGZ="Demangle_lib.tgz"
H_TGZ="Demangle_include.tgz"

curl -f -L -# "https://llvm.googlesource.com/llvm-project/+archive/${GIT_REF}/llvm/lib/Demangle.tar.gz" -o "${CC_TGZ}"
curl -f -L -# "https://llvm.googlesource.com/llvm-project/+archive/${GIT_REF}/llvm/include/llvm/Demangle.tar.gz" -o "${H_TGZ}"

mkdir -p "${CC_DIR}"
mkdir -p "${H_DIR}"

tar xf "${CC_TGZ}" -C "${CC_DIR}"
tar xf "${H_TGZ}" -C "${H_DIR}"

TAR_NAME="llvm-project-${GIT_REF}.tgz"
tar czf "${TAR_NAME}" --sort=name --owner=root:0 --group=root:0 --mtime='UTC 2019-01-01' llvm-project

TAR_SHA=$(sha256sum "${TAR_NAME}")

echo "output file: ${WORK_DIR}/${TAR_NAME}"
echo "contents sha256: ${TAR_SHA}"

popd
