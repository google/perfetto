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

import gzip

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto, PprofTextproto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import Tar, Zip


def _varint(n: int) -> bytes:
  out = bytearray()
  while True:
    bits = n & 0x7f
    n >>= 7
    if n:
      out.append(bits | 0x80)
    else:
      out.append(bits)
      return bytes(out)


def _field_varint(field_id: int, n: int) -> bytes:
  return _varint(field_id << 3) + _varint(n)


def _field_bytes(field_id: int, data: bytes) -> bytes:
  return _varint((field_id << 3) | 2) + _varint(len(data)) + data


def _mappingless_pprof() -> bytes:
  """A minimal gzipped pprof whose single location has no mapping.

  Locations without a resolvable mapping exercise the interned '[unknown]'
  fallback mapping in the pprof reader. Layout (profile.proto field ids):
  string_table(6), sample_type(1) {type(1), unit(2)}, function(5) {id(1),
  name(2)}, location(4) {id(1), line(4) {function_id(1)}}, sample(2)
  {location_id(1), value(2)}.
  """
  strings = [b'', b'cpu', b'nanoseconds', b'no_mapping_func']
  value_type = _field_varint(1, 1) + _field_varint(2, 2)
  function = _field_varint(1, 1) + _field_varint(2, 3)
  line = _field_varint(1, 1)
  location = _field_varint(1, 1) + _field_bytes(4, line)
  sample = _field_varint(1, 1) + _field_varint(2, 100)
  profile = (
      _field_bytes(1, value_type) + _field_bytes(2, sample) +
      _field_bytes(4, location) + _field_bytes(5, function) +
      b''.join(_field_bytes(6, s) for s in strings))
  return gzip.compress(profile)


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

  # Each member of an archive is scoped by its file name (the reader walks
  # past the unnamed gzip decompression layer to the nearest named ancestor).
  def test_pprof_zip_archive_scopes(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pprof': DataPath('pprof_simple_cpu.pprof'),
            'b.pprof': DataPath('pprof_multi_metric.pprof'),
        }),
        query="""
        SELECT scope, sample_type_type, sample_type_unit
        FROM __intrinsic_aggregate_profile
        ORDER BY scope, sample_type_type;
        """,
        out=Csv("""
        "scope","sample_type_type","sample_type_unit"
        "a.pprof","cpu","nanoseconds"
        "b.pprof","allocations","count"
        "b.pprof","cpu","nanoseconds"
        """))

  def test_pprof_tar_archive_scopes(self):
    return DiffTestBlueprint(
        trace=Tar({
            'a.pprof': DataPath('pprof_simple_cpu.pprof'),
            'b.pprof': DataPath('pprof_multi_metric.pprof'),
        }),
        query="""
        SELECT scope, sample_type_type, sample_type_unit
        FROM __intrinsic_aggregate_profile
        ORDER BY scope, sample_type_type;
        """,
        out=Csv("""
        "scope","sample_type_type","sample_type_unit"
        "a.pprof","cpu","nanoseconds"
        "b.pprof","allocations","count"
        "b.pprof","cpu","nanoseconds"
        """))

  # The same profile imported twice from an archive must not duplicate
  # frames, mappings or symbols: frames are interned per mapping and frames
  # that already carry a symbol_set_id are not re-symbolized. Only the
  # profiles/samples double.
  def test_pprof_zip_duplicate_file_interning(self):
    return DiffTestBlueprint(
        trace=Zip({
            'a.pprof': DataPath('pprof_simple_cpu.pprof'),
            'b.pprof': DataPath('pprof_simple_cpu.pprof'),
        }),
        query="""
        SELECT
          (SELECT COUNT(DISTINCT scope) FROM __intrinsic_aggregate_profile)
              AS scopes,
          (SELECT COUNT(*) FROM __intrinsic_aggregate_sample) AS samples,
          (SELECT COUNT(*) FROM stack_profile_frame) AS frames,
          (SELECT COUNT(*) FROM stack_profile_mapping
           WHERE name = '/proc/self/exe') AS exe_mappings,
          (SELECT COUNT(*) FROM stack_profile_symbol) AS symbols;
        """,
        out=Csv("""
        "scopes","samples","frames","exe_mappings","symbols"
        2,6,3,1,3
        """))

  # Locations without a resolvable mapping all share one interned '[unknown]'
  # fallback mapping, so identical frames dedupe across archive members.
  def test_pprof_zip_unknown_mapping_interned(self):
    pprof = _mappingless_pprof()
    return DiffTestBlueprint(
        trace=Zip({
            'a.pprof': pprof,
            'b.pprof': pprof,
        }),
        query="""
        SELECT
          (SELECT COUNT(*) FROM stack_profile_mapping
           WHERE name = '[unknown]') AS unknown_mappings,
          (SELECT COUNT(*) FROM stack_profile_frame) AS frames,
          (SELECT COUNT(DISTINCT scope) FROM __intrinsic_aggregate_profile)
              AS scopes,
          (SELECT SUM(value) FROM __intrinsic_aggregate_sample) AS total;
        """,
        out=Csv("""
        "unknown_mappings","frames","scopes","total"
        1,1,2,200.000000
        """))