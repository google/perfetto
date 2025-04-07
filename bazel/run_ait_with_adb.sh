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

function filter_adb_output() {
  # Version of adb used in tests may differ from the system one, so we filter
  # out lines related to the version mismatch and successfully daemon restarts.
  # The output can as follows, we want to keep only the last line:
  # adb server version (41) doesn't match this client (39); killing...
  # * daemon started successfully *
  # adb server version (41) doesn't match this client (39); killing...
  # * daemon started successfully *
  # Failure [DELETE_FAILED_INTERNAL_ERROR]
  echo -n "$1" | grep -v "^adb server version (" | grep -v "^* daemon started successfully"
}

function call_adb_uninstall() {
  local NO_PACKAGE_INSTALLED="Failure [DELETE_FAILED_INTERNAL_ERROR]"
  readonly NO_PACKAGE_INSTALLED
  local apk_pkg="$1"

  local OUTPUT
  # adb uninstall exits with error if there is no package 'apk_pkg' installed,
  # this is fine for us: we try to delete a package first even if it is not
  # installed.
  set +e
  OUTPUT=$(call_adb uninstall "${apk_pkg}" 2>&1)
  set -e

  local FILTERED_OUTPUT
  FILTERED_OUTPUT=$(filter_adb_output "$OUTPUT")
  if [[ "$FILTERED_OUTPUT" != "$NO_PACKAGE_INSTALLED" ]] && [[ "$FILTERED_OUTPUT" != "Success" ]]; then
    # Print the whole output
    echo "adb uninstall error: '${OUTPUT}'"
    exit 1
  fi
}

function check_connected_devices() {
  local DEVICES_OUTPUT
  local FILTERED_DEVICES_OUTPUT
  DEVICES_OUTPUT=$(call_adb devices)
  FILTERED_DEVICES_OUTPUT=$(filter_adb_output "$DEVICES_OUTPUT")
  # Expected filtered output:
  # List of devices attached
  # emulator-5554   device
  if [[ "$FILTERED_DEVICES_OUTPUT" == "List of devices attached" ]]; then
    # If output contains only header line then there is no connected devices
    echo "Test Error: No connected devices"
    exit 1
  fi
}

check_connected_devices

call_adb_uninstall "${apk_package}"

call_adb_uninstall "${test_apk_package}"

call_adb install "${apk_path}"

call_adb install "${test_apk_path}"

TEST_OUTPUT=$(call_adb shell am instrument -w "${test_apk_package}/${TEST_RUNNER_CLASS}")

echo "'adb shell am instrument' output:"
echo "${TEST_OUTPUT}"

# TODO(ktimofeev): Run 'am instrument' with '-r' flag and parse output.
if [[ "${TEST_OUTPUT}" =~ "FAILURES!!!" ]] ||
   [[ "${TEST_OUTPUT}" =~ "INSTRUMENTATION_RESULT: shortMsg=Process crashed" ]] ||
   [[ "${TEST_OUTPUT}" =~ "INSTRUMENTATION_FAILED" ]]; then
  exit 1
fi