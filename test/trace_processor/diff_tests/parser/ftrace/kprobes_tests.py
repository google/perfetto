#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Kprobes(TestSuite):

  def test_kprobes_slice(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { ftrace_events {
          cpu: 1
          event {
            timestamp: 1500
            pid: 42
            kprobe_event {
              name: "fuse_file_write_iter"
              type: KPROBE_TYPE_BEGIN
            }
          }
          event {
            timestamp: 2700
            pid: 42
            kprobe_event {
              name: "fuse_file_write_iter"
              type: KPROBE_TYPE_END
            }
          }
        }}
        """),
        query="""
        select
          ts,
          dur as slice_dur,
          slice.name as slice_name
        from slice
        """,
        out=Csv("""
        "ts","slice_dur","slice_name"
        1500,1200,"fuse_file_write_iter"
        """))

  def test_kprobes_instant(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { ftrace_events {
          cpu: 1
          event {
            timestamp: 1500
            pid: 42
            kprobe_event {
              name: "fuse_file_write_iter"
              type: KPROBE_TYPE_INSTANT
            }
          }
        }}
        """),
        query="""
        select
          ts,
          dur as slice_dur,
          slice.name as slice_name
        from slice
        """,
        out=Csv("""
        "ts","slice_dur","slice_name"
        1500,0,"fuse_file_write_iter"
        """))
