#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class BlockIo(TestSuite):

  def test_block_io_slice(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet { ftrace_events {
          cpu: 1
          event {
            timestamp: 1000
            pid: 31918
            block_io_start {
              dev: 45824
              sector: 44920176
              nr_sector: 56
              bytes: 28672
              ioprio: 16386
              rwbs: "RA"
              comm: "oid.apps.chrome"
              cmd: ""
            }
          }
          event {
            timestamp: 2400
            pid: 0
            block_io_done {
              dev: 45824
              sector: 44920176
              nr_sector: 0
              bytes: 0
              ioprio: 16386
              rwbs: "RA"
              comm: "kworker/0:2H"
              cmd: ""
            }
          }
        }}
        """),
        query="""
        SELECT
          slice.name as name,
          slice.ts as ts,
          slice.dur as dur,
          extract_arg(track.dimension_arg_set_id, 'block_device') as dev,
          extract_arg(slice.arg_set_id, 'sector') as sector
        FROM slice
        JOIN track ON slice.track_id = track.id AND track.type = 'block_io'
        """,
        out=Csv("""
        "name","ts","dur","dev","sector"
        "block_io",1000,1400,45824,44920176
        """))
