#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License a
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class AndroidFs(TestSuite):

  # android_fs_dataread
  def test_android_fs_dataread(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 46448185788840
              pid: 5892
             android_fs_dataread_start {
                bytes: 4096
                pid: 5892
                ino: 836
                offset: 0
                cmdline: "am"
                i_size: 31772
                pathbuf: "/system/bin/cmd"
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 46448185789530
              pid: 156
              android_fs_dataread_end {
                bytes: 4096
                ino: 836
                offset: 0
              }
            }
          }
        }
        """),
        query="""
        SELECT ts, dur, name FROM slice WHERE name = 'android_fs_data_read';
        """,
        out=Csv("""
        "ts","dur","name"
        46448185788840,690,"android_fs_data_read"
        """))
