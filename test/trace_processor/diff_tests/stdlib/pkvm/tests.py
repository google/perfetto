#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Pkvm(TestSuite):

  def test_pkvm_hypervisor_events(self):
    return DiffTestBlueprint(
        trace=Path('pkvm_hypervisor_events.textproto'),
        query="""
        INCLUDE PERFETTO MODULE pkvm.hypervisor;
        SELECT
          cpu,
          ts,
          dur,
          reason
        FROM
          pkvm_hypervisor_events
        ORDER BY cpu, ts, dur, reason;
        """,
        out=Csv("""
        "cpu","ts","dur","reason"
        0,84798810112835,936,"host_smc"
        0,84798810182293,285,"[NULL]"
        1,84798810660565,11190,"host_mem_abort"
        1,84798811118003,3703,"host_hcall"
        """))

  def test_pkvm_hypervisor_host_hcall(self):
    return DiffTestBlueprint(
        trace=Path('pkvm_hypervisor_events.textproto'),
        query="""
        INCLUDE PERFETTO MODULE pkvm.hypervisor;
        SELECT
          pkvm_hyp.cpu as cpu,
          pkvm_hyp.ts as ts,
          pkvm_hyp.dur as dur,
          EXTRACT_ARG(slices.arg_set_id, 'id') as id,
          EXTRACT_ARG(slices.arg_set_id, 'invalid') as invalid
        FROM
          pkvm_hypervisor_events as pkvm_hyp
        JOIN slices
        ON pkvm_hyp.slice_id = slices.id
        WHERE
          reason = "host_hcall"
        ORDER BY cpu, ts, dur, id, invalid;
        """,
        out=Csv("""
        "cpu","ts","dur","id","invalid"
        1,84798811118003,3703,2818048,0
        """))

  def test_pkvm_hypervisor_host_smc(self):
    return DiffTestBlueprint(
        trace=Path('pkvm_hypervisor_events.textproto'),
        query="""
        INCLUDE PERFETTO MODULE pkvm.hypervisor;
        SELECT
          pkvm_hyp.cpu as cpu,
          pkvm_hyp.ts as ts,
          pkvm_hyp.dur as dur,
          EXTRACT_ARG(slices.arg_set_id, 'id') as id,
          EXTRACT_ARG(slices.arg_set_id, 'forwarded') as forwarded
        FROM
          pkvm_hypervisor_events as pkvm_hyp
        JOIN slices
        ON pkvm_hyp.slice_id = slices.id
        WHERE
          reason = "host_smc"
        ORDER BY cpu, ts, dur, id, forwarded;
        """,
        out=Csv("""
        "cpu","ts","dur","id","forwarded"
        0,84798810112835,936,281474976710656,0
        """))

  def test_pkvm_hypervisor_host_mem_abort(self):
    return DiffTestBlueprint(
        trace=Path('pkvm_hypervisor_events.textproto'),
        query="""
        INCLUDE PERFETTO MODULE pkvm.hypervisor;
        SELECT
          pkvm_hyp.cpu as cpu,
          pkvm_hyp.ts as ts,
          pkvm_hyp.dur as dur,
          EXTRACT_ARG(slices.arg_set_id, 'esr') as esr,
          EXTRACT_ARG(slices.arg_set_id, 'addr') as addr
        FROM
          pkvm_hypervisor_events as pkvm_hyp
        JOIN slices
        ON pkvm_hyp.slice_id = slices.id
        WHERE
          reason = "host_mem_abort"
        ORDER BY cpu, ts, dur, esr, addr;
        """,
        out=Csv("""
        "cpu","ts","dur","esr","addr"
        1,84798810660565,11190,1970324836974592,-4810970301938499072
        """))

  # hyp_enter while a hyp slice is already open: the stale slice is closed with
  # an unknown duration (-1) and the new slice starts flat (no nesting). A
  # hyp_exit without an open slice is ignored.
  def test_pkvm_hypervisor_bad_events(self):
    return DiffTestBlueprint(
        trace=Path('pkvm_hypervisor_bad_events.textproto'),
        query="""
        INCLUDE PERFETTO MODULE pkvm.hypervisor;
        SELECT
          pkvm_hyp.cpu AS cpu,
          pkvm_hyp.ts AS ts,
          pkvm_hyp.dur AS dur,
          slices.depth AS depth
        FROM
          pkvm_hypervisor_events AS pkvm_hyp
        JOIN slices
        ON pkvm_hyp.slice_id = slices.id
        ORDER BY cpu, ts;
        """,
        out=Csv("""
        "cpu","ts","dur","depth"
        0,1000,-1,0
        0,2000,1000,0
        0,5000,1000,0
        """))

  def test_pkvm_hypervisor_bad_events_stats(self):
    return DiffTestBlueprint(
        trace=Path('pkvm_hypervisor_bad_events.textproto'),
        query="""
        SELECT name, severity, source, value
        FROM stats
        WHERE name GLOB 'pkvm_hyp_*'
        ORDER BY name;
        """,
        out=Csv("""
        "name","severity","source","value"
        "pkvm_hyp_enter_while_open","data_loss","analysis",1
        "pkvm_hyp_exit_without_enter","data_loss","analysis",1
        """))
