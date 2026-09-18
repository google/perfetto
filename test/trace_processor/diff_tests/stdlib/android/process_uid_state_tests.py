# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, DiffTestBlueprint, TestSuite, Path


class ProcessUidState(TestSuite):

  def test_process_uid_state(self):
    return DiffTestBlueprint(
        trace=Path('process_uid_state_data.py'),
        query="""
        INCLUDE PERFETTO MODULE android.process_uid_state;
        SELECT ts, dur, uid, process_state_name
        FROM android_process_uid_state;
        """,
        out=Csv("""
        "ts","dur","uid","process_state_name"
        1000,4000,10001,"PROCESS_STATE_FOREGROUND_SERVICE"
        5000,0,10001,"PROCESS_STATE_TOP"
        """))
