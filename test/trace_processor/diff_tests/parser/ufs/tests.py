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


class Ufs(TestSuite):
  # UFS command
  def test_ufshcd_command(self):
    return DiffTestBlueprint(
        trace=Path('ufshcd_command.textproto'),
        query="""
        SELECT
          ts,
          value
        FROM
          counter AS c
        JOIN
          counter_track AS ct
          ON c.track_id = ct.id
        WHERE
          ct.name = "io.ufs.command.count"
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","value"
        10000,1.000000
        10008,2.000000
        10010,3.000000
        10011,1.000000
        """))

  def test_ufshcd_command_tag(self):
    return DiffTestBlueprint(
        trace=Path('ufshcd_command_tag.textproto'),
        query="""
        SELECT ts, dur, slice.name
        FROM slice
        JOIN track ON slice.track_id = track.id
        WHERE track.name GLOB 'io.ufs.command.tag*'
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","dur","name"
        10000,800,"READ (10)"
        10900,50,"WRITE (10) (GID=0x16)"
        """))
