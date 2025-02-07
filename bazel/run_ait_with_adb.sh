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

set -e -o pipefail

# we pass "--adb_flags=<>" option from Bazel test invocation, e.g.:
# bazel test :my_instrumentation_test --test_arg --adb_flags="-s emulator-5554"
# Bazel passes this option into the args of this script.
# We support only one option, named "adb_flags" and validate it here.
ADB_FLAGS=""
if [[ -n "$1" ]]; then
  ARG_PREFIX="--adb_flags="
  ARG_PREFIX_REGEX="^$ARG_PREFIX"
  if [[ "$1" =~ $ARG_PREFIX_REGEX ]]; then
    ARG="$1"
    ADB_FLAGS=${ARG#*"$ARG_PREFIX"}
  else
    echo "Unexpected script argument '$1'"
    exit 1
  fi
fi

readonly ADB_FLAGS

readonly adb_tool_path="%%adb_tool_path%%"
readonly appt2_tool_path="%%appt2_tool_path%%"

readonly apk_path="%%apk_path%%"
readonly test_apk_path="%%test_apk_path%%"

readonly TEST_RUNNER_CLASS="androidx.test.runner.AndroidJUnitRunner"

function call_adb() {
  # we don't wrap $ADB_FLAGS in quotes, since it may contains a list of strings
  # (e.g "-s emulator-5554"), and we want it to expand to the list of strings
  # when passing it as args to the adb tool.
  # shellcheck disable=SC2086
  "${adb_tool_path}" $ADB_FLAGS "$@"
}

apk_package=$("${appt2_tool_path}" dump packagename "${apk_path}")
test_apk_package=$("${appt2_tool_path}" dump packagename "${test_apk_path}")


function call_adb_uninstall() {
  local NO_PACKAGE_INSTALLED="Failure [DELETE_FAILED_INTERNAL_ERROR]"
  readonly NO_PACKAGE_INSTALLED
  local apk_pkg="$1"
  # adb uninstall exits with error if there is no package 'apk_pkg' installed,
  # this is fine for us: we try to delete a package first even if it is not
  # installed.
  set +e

  local res
  res=$(call_adb uninstall "${apk_pkg}" 2>&1)
  if [[ "$res" != "$NO_PACKAGE_INSTALLED" ]] && [[ "$res" != "Success" ]]; then
    echo "adb uninstall error: '${res}'"
    exit 1
  fi

  set -e
}

call_adb_uninstall "${apk_package}"

call_adb_uninstall "${test_apk_package}"

call_adb install "${apk_path}"

call_adb install "${test_apk_path}"

TEST_OUTPUT=$(call_adb shell am instrument -w "${test_apk_package}/${TEST_RUNNER_CLASS}")

echo "${TEST_OUTPUT}"

if [[ "${TEST_OUTPUT}" =~ "FAILURES!!!" ]]; then
  exit 1
fi