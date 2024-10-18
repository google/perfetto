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
        SELECT
          EXTRACT_ARG(
            dimension_arg_set_id,
            'energy_consumer_id'
          ) AS consumer_id,
          name,
          EXTRACT_ARG(source_arg_set_id, 'consumer_type') AS consumer_type,
          EXTRACT_ARG(source_arg_set_id, 'ordinal') AS ordinal
        FROM track
        WHERE classification = 'android_energy_estimation_breakdown';
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
        JOIN track ON counter.track_id = track.id
        ORDER BY ts;
        """,
        out=Csv("""
        "ts","value"
        1030255882785,98567522.000000
        """))

  def test_energy_per_uid_table(self):
    return DiffTestBlueprint(
        trace=Path('energy_breakdown_uid.textproto'),
        query="""
        SELECT
          EXTRACT_ARG(
            dimension_arg_set_id,
            'energy_consumer_id'
          ) AS consumer_id,
          EXTRACT_ARG(dimension_arg_set_id, 'uid') AS uid
        FROM track
        WHERE classification = 'android_energy_estimation_breakdown_per_uid';
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
        SELECT
          EXTRACT_ARG(dimension_arg_set_id, 'uid') AS uid,
          name
        FROM track
        WHERE classification = 'android_energy_estimation_breakdown_per_uid'
          AND machine_id IS NOT NULL;
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
        SELECT
          EXTRACT_ARG(
            dimension_arg_set_id,
            'energy_consumer_id'
          ) AS consumer_id,
          name,
          EXTRACT_ARG(source_arg_set_id, 'consumer_type') AS consumer_type,
          EXTRACT_ARG(source_arg_set_id, 'ordinal') AS ordinal
        FROM track
        WHERE classification = 'android_energy_estimation_breakdown'
          AND machine_id IS NOT NULL;
        """,
        out=Csv("""
        "consumer_id","name","consumer_type","ordinal"
        0,"CPUCL0","CPU_CLUSTER",0
        """))
