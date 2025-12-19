#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto, PprofTextproto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class PprofParser(TestSuite):

  def test_pprof_simple_cpu_import(self):
    return DiffTestBlueprint(
        trace=DataPath('pprof_simple_cpu.pprof'),
        query="""
        SELECT scope, sample_type_type, sample_type_unit
        FROM __intrinsic_aggregate_profile
        ORDER BY scope, sample_type_type;
        """,
        out=Csv("""
        "scope","sample_type_type","sample_type_unit"
        "pprof_file","cpu","nanoseconds"
        """))

  def test_pprof_simple_cpu_samples(self):
    return DiffTestBlueprint(
        trace=DataPath('pprof_simple_cpu.pprof'),
        query="""
        SELECT COUNT(*) as sample_count
        FROM __intrinsic_aggregate_sample sample
        JOIN __intrinsic_aggregate_profile profile ON sample.aggregate_profile_id = profile.id
        WHERE profile.sample_type_type = 'cpu';
        """,
        out=Csv("""
        "sample_count"
        3
        """))

  def test_pprof_simple_cpu_flamegraph_data(self):
    return DiffTestBlueprint(
        trace=DataPath('pprof_simple_cpu.pprof'),
        query="""
        SELECT frame.name, sample.value
        FROM __intrinsic_aggregate_sample sample
        JOIN __intrinsic_aggregate_profile profile ON sample.aggregate_profile_id = profile.id
        JOIN stack_profile_callsite cs ON sample.callsite_id = cs.id
        JOIN stack_profile_frame frame ON cs.frame_id = frame.id
        WHERE profile.sample_type_type = 'cpu'
        ORDER BY sample.value DESC;
        """,
        out=Csv("""
        "name","value"
        "main",2000000.000000
        "main",1000000.000000
        "foo",500000.000000
        """))

  def test_pprof_multi_metric_import(self):
    return DiffTestBlueprint(
        trace=DataPath('pprof_multi_metric.pprof'),
        query="""
        SELECT scope, sample_type_type, sample_type_unit
        FROM __intrinsic_aggregate_profile
        ORDER BY scope, sample_type_type;
        """,
        out=Csv("""
        "scope","sample_type_type","sample_type_unit"
        "pprof_file","allocations","count"
        "pprof_file","cpu","nanoseconds"
        """))

  def test_pprof_multi_metric_cpu_values(self):
    return DiffTestBlueprint(
        trace=DataPath('pprof_multi_metric.pprof'),
        query="""
        SELECT SUM(sample.value) as total_cpu_ns
        FROM __intrinsic_aggregate_sample sample
        JOIN __intrinsic_aggregate_profile profile ON sample.aggregate_profile_id = profile.id
        WHERE profile.sample_type_type = 'cpu';
        """,
        out=Csv("""
        "total_cpu_ns"
        4500000.000000
        """))

  def test_pprof_multi_metric_allocation_values(self):
    return DiffTestBlueprint(
        trace=DataPath('pprof_multi_metric.pprof'),
        query="""
        SELECT SUM(sample.value) as total_allocations
        FROM __intrinsic_aggregate_sample sample
        JOIN __intrinsic_aggregate_profile profile ON sample.aggregate_profile_id = profile.id
        WHERE profile.sample_type_type = 'allocations';
        """,
        out=Csv("""
        "total_allocations"
        17.000000
        """))

  def test_pprof_stack_profile_integration(self):
    return DiffTestBlueprint(
        trace=DataPath('pprof_simple_cpu.pprof'),
        query="""
        SELECT
          frame.name,
          mapping.name as mapping_name,
          COUNT(*) as callsite_count
        FROM __intrinsic_aggregate_sample sample
        JOIN stack_profile_callsite cs ON sample.callsite_id = cs.id
        JOIN stack_profile_frame frame ON cs.frame_id = frame.id
        JOIN stack_profile_mapping mapping ON frame.mapping = mapping.id
        GROUP BY frame.name, mapping.name
        ORDER BY frame.name;
        """,
        out=Csv("""
        "name","mapping_name","callsite_count"
        "foo","/proc/self/exe",1
        "main","/proc/self/exe",2
        """))