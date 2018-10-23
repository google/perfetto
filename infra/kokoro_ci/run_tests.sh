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

set -ex

# Trivial placeholder script while wiring up the CI.

SCRIPT_DIR="$(realpath "$(dirname "${BASH_SOURCE[0]}")")"
ROOT_DIR="$(realpath ${SCRIPT_DIR}/../..)"

cd ${ROOT_DIR}

echo Kokoro CI configuration and ACLs look ok
ls
