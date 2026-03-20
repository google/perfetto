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

from python.generators.diff_tests.testing import DataPath
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ArtHprofParser(TestSuite):

  def test_art_hprof_class_count_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() FROM heap_graph_class
        """,
        out=Csv('''
          "COUNT()"
          2252
        '''))

  def test_art_hprof_object_count_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() FROM heap_graph_object
        """,
        out=Csv('''
          "COUNT()"
          25919
        '''))

  def test_art_hprof_primitives(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            f.field_name,
            f.field_type,
            COALESCE(
              CAST(bool_value AS TEXT),
              CAST(byte_value AS TEXT),
              CAST(char_value AS TEXT),
              CAST(short_value AS TEXT),
              CAST(int_value AS TEXT),
              CAST(long_value AS TEXT),
              printf('%f', float_value),
              printf('%f', double_value)
            ) as val
          FROM heap_graph_primitive f
          JOIN heap_graph_object o ON f.object_id = o.id
          JOIN heap_graph_class c ON o.type_id = c.id
          WHERE (c.name = 'java.lang.Boolean' AND f.field_name = 'java.lang.Boolean.value')
             OR (c.name = 'java.lang.Character' AND f.field_name = 'java.lang.Character.value')
             OR (c.name = 'java.util.HashMap' AND f.field_name = 'java.util.HashMap.loadFactor')
             OR (c.name = 'java.lang.Class<java.lang.Float>' AND f.field_name = 'POSITIVE_INFINITY')
             OR (c.name = 'java.lang.Class<java.lang.Float>' AND f.field_name = 'NaN')
          GROUP BY 1, 2, 3
          ORDER BY f.field_name, val
          LIMIT 10
        """,
        out=Csv('''
          "field_name","field_type","val"
          "NaN","float","0.000000"
          "POSITIVE_INFINITY","float","Inf"
          "java.lang.Boolean.value","boolean","0"
          "java.lang.Boolean.value","boolean","1"
          "java.lang.Character.value","char","0"
          "java.lang.Character.value","char","1"
          "java.lang.Character.value","char","10"
          "java.lang.Character.value","char","100"
          "java.lang.Character.value","char","101"
          "java.lang.Character.value","char","102"
        '''))

  def test_art_hprof_strings(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT od.value_string
          FROM heap_graph_object o
          JOIN heap_graph_class c ON o.type_id = c.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE c.name = 'java.lang.String'
            AND od.value_string IN ('!/', '$Proxy', 'java.lang.String')
          GROUP BY 1
          ORDER BY 1
        """,
        out=Csv('''
          "value_string"
          "!/"
          "$Proxy"
          "java.lang.String"
        '''))

  def test_art_hprof_array_json(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            od.array_element_type,
            od.array_element_count,
            length(__intrinsic_heap_graph_get_array(od.array_data_id)) as blob_len,
            __intrinsic_heap_graph_get_array_json(
              od.array_data_id, od.array_element_type, od.array_element_count
            ) as json_val
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name IN ('DumpedStuff.K', 'DumpedStuff.i', 'DumpedStuff.g')
          ORDER BY od.array_element_type
        """,
        out=Csv('''
          "array_element_type","array_element_count","blob_len","json_val"
          "byte",6,6,"[0,1,2,3,4,5]"
          "char",10,20,"[99,104,97,114,32,116,104,105,110,103]"
          "int",4,16,"[3,1,2,0]"
        '''))

  def test_art_hprof_array_blob(self):
    """Verify raw blob content for a primitive array."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            hex(__intrinsic_heap_graph_get_array(od.array_data_id)) as blob_hex
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name = 'DumpedStuff.i'
        """,
        out=Csv('''
          "blob_hex"
          "000102030405"
        '''))

  def test_art_hprof_array_hashing(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            (SELECT od.array_data_hash
             FROM heap_graph_reference r
             JOIN heap_graph_object owner ON r.owner_id = owner.id
             JOIN heap_graph_class oc ON owner.type_id = oc.id
             JOIN heap_graph_object o ON r.owned_id = o.id
             JOIN heap_graph_object_data od ON od.object_id = o.id
             WHERE oc.name = 'DumpedStuff' AND r.field_name = 'DumpedStuff.K') as known_hash,
            (SELECT COUNT(DISTINCT array_data_hash) FROM heap_graph_object_data) <
            (SELECT COUNT(*) FROM heap_graph_object_data WHERE array_data_id IS NOT NULL) as has_duplicates
        """,
        out=Csv('''
          "known_hash","has_duplicates"
          -3333146854241245275,1
        '''))

  def test_art_hprof_intrinsics_edge_cases(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            __intrinsic_heap_graph_get_array(NULL) IS NULL as null_blob,
            __intrinsic_heap_graph_get_array_json(NULL, 'int', 0) IS NULL as null_json
        """,
        out=Csv('''
          "null_blob","null_json"
          1,1
        '''))
