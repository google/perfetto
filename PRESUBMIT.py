# Copyright (C) 2017 The Android Open Source Project
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

def CheckChange(input, output):
    results = []
    results += input.canned_checks.CheckDoNotSubmit(input, output)
    results += input.canned_checks.CheckChangeHasNoTabs(input, output)
    results += input.canned_checks.CheckLongLines(input, output, 80)
    results += input.canned_checks.CheckPatchFormatted(input, output)
    results += input.canned_checks.CheckGNFormatted(input, output)
    return results

def CheckChangeOnUpload(input_api, output_api):
    return CheckChange(input_api, output_api)

def CheckChangeOnCommit(input_api, output_api):
    return CheckChange(input_api, output_api)

