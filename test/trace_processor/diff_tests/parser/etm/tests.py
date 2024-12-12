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

from python.generators.diff_tests.testing import Csv, Path, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Etm(TestSuite):

  def test_sessions(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        query='''
          SELECT start_ts, cpu, size
          FROM
            __intrinsic_etm_v4_configuration AS C,
             __intrinsic_etm_v4_session AS s
             ON c.id = s.configuration_id,
             __intrinsic_etm_v4_trace AS t
             ON s.id = t.session_id
          WHERE start_ts < 21077000721310
          ORDER BY start_ts ASC
        ''',
        out=Csv('''
         "start_ts","cpu","size"
          21076849718299,8,238
          21076860480953,8,238
          21076871238359,8,238
          21076882058915,8,238
          21076892817338,8,238
          21076903628740,8,238
          21076914454749,8,238
          21076924779741,8,238
          21076935681637,8,238
          21076946070594,8,360
          21076956953243,8,238
          21076967922726,8,238
          21076978919593,8,238
          21076989801225,8,356
        '''))

  def test_decode_all(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        query='''
          SELECT count(*)
          FROM
            __intrinsic_etm_v4_trace t,
            __intrinsic_etm_decode_trace d
            ON t.id = d.trace_id
        ''',
        out=Csv('''
          "count(*)"
          5871
        '''))

  def test_decode_trace(self):
    return DiffTestBlueprint(
        trace=DataPath('simpleperf/cs_etm_u.perf'),
        query='''
          SELECT *
          FROM
            __intrinsic_etm_decode_trace
          WHERE trace_id = 0
        ''',
        out=Csv('''
          "trace_index","element_index","element_type","timestamp","cycle_count","exception_level","context_id","isa","start_address","end_address","mapping_id"
          12,0,"NO_SYNC","[NULL]","[NULL]","[NULL]","[NULL]","UNKNOWN",-1,-1,"[NULL]"
          40,1,"TRACE_ON","[NULL]","[NULL]","[NULL]","[NULL]","UNKNOWN",-1,-1,"[NULL]"
          40,2,"PE_CONTEXT","[NULL]","[NULL]",0,315,"AARCH64",-1,-1,"[NULL]"
          40,3,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487860616,-1,13
          50,4,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487537936,-1,13
          60,5,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",434500225096,-1,1
          70,6,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487560912,-1,13
          76,7,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487493696,-1,13
          85,8,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487561064,-1,13
          91,9,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487412480,-1,13
          97,10,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487818588,-1,13
          107,11,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487819616,-1,13
          112,12,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487821296,-1,13
          130,13,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487386304,-1,13
          137,14,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487799440,-1,13
          144,15,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487387072,-1,13
          152,16,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487799592,-1,13
          159,17,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487386304,-1,13
          166,18,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487799440,-1,13
          173,19,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487387072,-1,13
          180,20,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487799592,-1,13
          187,21,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487827048,-1,13
          199,22,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487386304,-1,13
          206,23,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487799440,-1,13
          213,24,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487387072,-1,13
          221,25,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487799592,-1,13
          229,26,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487548832,-1,13
          235,27,"ADDR_NACC","[NULL]","[NULL]",0,315,"AARCH64",523487677904,-1,13
        '''))