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
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class PowerEnergyBreakdown(TestSuite):
  # Energy Estimation Breakdown
  def test_energy_breakdown_table(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown.textproto'),
        query="""
        SELECT consumer_id, name, consumer_type, ordinal
        FROM energy_counter_track;
        """,
        out=Csv("""
        "consumer_id","name","consumer_type","ordinal"
        0,"CPUCL0","CPU_CLUSTER",0
        """))

  def test_energy_breakdown_event(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown.textproto'),
        query="""
        SELECT ts, value
        FROM counter
        JOIN energy_counter_track ON counter.track_id = energy_counter_track.id
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","value"
        1030255882785,98567522.000000
        """))

  def test_energy_breakdown_uid_table(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query="""
        SELECT uid, name
        FROM uid_counter_track;
        """,
        out=Csv("""
        "uid","name"
        10234,"GPU"
        10190,"GPU"
        10235,"GPU"
        """))

  def test_energy_breakdown_uid_event(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query="""
        SELECT ts, value
        FROM counter
        JOIN uid_counter_track ON counter.track_id = uid_counter_track.id
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","value"
        1026753926322,3004536.000000
        1026753926322,0.000000
        1026753926322,4002274.000000
        """))

  def test_energy_per_uid_table(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query="""
        SELECT consumer_id, uid
        FROM energy_per_uid_counter_track;
        """,
        out=Csv("""
        "consumer_id","uid"
        3,10234
        3,10190
        3,10235
        """))

  def test_energy_breakdown_uid_table_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        trace_modifier=TraceInjector(['android_energy_estimation_breakdown'],
                                     {'machine_id': 1001}),
        query="""
        SELECT uid, name
        FROM uid_counter_track
        WHERE machine_id IS NOT NULL;
        """,
        out=Csv("""
        "uid","name"
        10234,"GPU"
        10190,"GPU"
        10235,"GPU"
        """))

  def test_energy_breakdown_table_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown.textproto'),
        trace_modifier=TraceInjector(['android_energy_estimation_breakdown'],
                                     {'machine_id': 1001}),
        query="""
        SELECT consumer_id, name, consumer_type, ordinal
        FROM energy_counter_track
        WHERE machine_id IS NOT NULL;
        """,
        out=Csv("""
        "consumer_id","name","consumer_type","ordinal"
        0,"CPUCL0","CPU_CLUSTER",0
        """))
