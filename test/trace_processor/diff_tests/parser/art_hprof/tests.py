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

  def test_art_hprof_class_examples_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT name, kind FROM heap_graph_class
          ORDER BY name
          LIMIT 10
        """,
        out=Csv('''
          "name","kind"
          "DumpedStuff","[unknown class kind]"
          "Main","[unknown class kind]"
          "SuperDumpedStuff","[unknown class kind]"
          "a","[unknown class kind]"
          "a.a","[unknown class kind]"
          "a.b","[unknown class kind]"
          "a.c","[unknown class kind]"
          "android.compat.Compatibility","[unknown class kind]"
          "android.graphics.a","[unknown class kind]"
          "android.graphics.b","[unknown class kind]"
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

  def test_art_hprof_object_examples_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            graph_sample_ts,
            self_size,
            native_size,
            reachable,
            heap_type,
            root_type,
            root_distance
          FROM heap_graph_object
          ORDER BY self_size DESC
          LIMIT 10
        """,
        out=Csv('''
          "graph_sample_ts","self_size","native_size","reachable","heap_type","root_type","root_distance"
          1740172787560,1000012,0,1,"app","[NULL]",2
          1740172787560,16396,0,1,"app","[NULL]",2
          1740172787560,8204,0,1,"app","[NULL]",4
          1740172787560,3300,0,1,"app","[NULL]",1
          1740172787560,2388,0,1,"app","STICKY_CLASS",0
          1740172787560,1444,0,1,"app","STICKY_CLASS",0
          1740172787560,1412,0,1,"app","[NULL]",1
          1740172787560,1264,0,1,"app","[NULL]",1
          1740172787560,1248,0,1,"app","[NULL]",1
          1740172787560,1248,0,1,"app","[NULL]",1
        '''))

  def test_art_hprof_reference_type_kinds(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT kind, count(*) AS cnt FROM heap_graph_class
          WHERE kind != '[unknown class kind]'
          GROUP BY kind ORDER BY kind
        """,
        out=Csv('''
          "kind","cnt"
          "KIND_FINALIZER_REFERENCE",1
          "KIND_PHANTOM_REFERENCE",4
          "KIND_SOFT_REFERENCE",2
          "KIND_WEAK_REFERENCE",9
        '''))

  def test_art_hprof_reference_count_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() FROM heap_graph_reference
        """,
        out=Csv('''
                "COUNT()"
                53937
        '''))

  def test_art_hprof_reference_examples_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT r.field_name, r.field_type_name, owner_class.name, owned_class.name
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class owner_class ON owner.type_id = owner_class.id
          JOIN heap_graph_object owned ON r.owned_id = owned.id
          JOIN heap_graph_class owned_class ON owned.type_id = owned_class.id
          ORDER BY 1, 2, 3, 4
          LIMIT 10
        """,
        out=Csv('''
          "field_name","field_type_name","name","name"
          "DumpedStuff.$class$classLoader","dalvik.system.PathClassLoader","java.lang.Class<DumpedStuff>","dalvik.system.PathClassLoader"
          "DumpedStuff.$class$dexCache","java.lang.DexCache","java.lang.Class<DumpedStuff>","java.lang.DexCache"
          "DumpedStuff.$class$ifTable","java.lang.Object[]","java.lang.Class<DumpedStuff>","java.lang.Object[]"
          "DumpedStuff.$class$shadow$_klass_","java.lang.Class","java.lang.Class<DumpedStuff>","java.lang.Class<java.lang.Class>"
          "DumpedStuff.$class$superClass","SuperDumpedStuff","java.lang.Class<DumpedStuff>","java.lang.Class<SuperDumpedStuff>"
          "DumpedStuff.$classOverhead","byte[]","java.lang.Class<DumpedStuff>","byte[]"
          "DumpedStuff.A","java.lang.ref.WeakReference","DumpedStuff","java.lang.ref.WeakReference"
          "DumpedStuff.B","java.lang.ref.WeakReference","DumpedStuff","java.lang.ref.WeakReference"
          "DumpedStuff.C","java.lang.ref.SoftReference","DumpedStuff","java.lang.ref.SoftReference"
          "DumpedStuff.D","java.lang.Object[]","DumpedStuff","java.lang.Object[]"
        '''))

  def test_art_hprof_primitive_count_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() FROM heap_graph_primitive
        """,
        out=Csv('''
          "COUNT()"
          47300
        '''))

  def test_art_hprof_primitive_type_distribution(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT field_type, COUNT() as cnt
          FROM heap_graph_primitive
          GROUP BY field_type
          ORDER BY cnt DESC
        """,
        out=Csv('''
          "field_type","cnt"
          "int",40751
          "long",3078
          "short",2532
          "byte",397
          "boolean",312
          "char",160
          "float",44
          "double",26
        '''))

  def test_art_hprof_primitive_value_integrity(self):
    """Verify each row has at most one value column set."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as rows_with_multiple_values
          FROM heap_graph_primitive
          WHERE (CASE WHEN bool_value IS NOT NULL THEN 1 ELSE 0 END
               + CASE WHEN byte_value IS NOT NULL THEN 1 ELSE 0 END
               + CASE WHEN char_value IS NOT NULL THEN 1 ELSE 0 END
               + CASE WHEN short_value IS NOT NULL THEN 1 ELSE 0 END
               + CASE WHEN int_value IS NOT NULL THEN 1 ELSE 0 END
               + CASE WHEN long_value IS NOT NULL THEN 1 ELSE 0 END
               + CASE WHEN float_value IS NOT NULL THEN 1 ELSE 0 END
               + CASE WHEN double_value IS NOT NULL THEN 1 ELSE 0 END) > 1
        """,
        out=Csv('''
          "rows_with_multiple_values"
          0
        '''))

  def test_art_hprof_primitive_nonnull_per_type(self):
    """Verify non-null value counts per typed column."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            SUM(CASE WHEN bool_value IS NOT NULL THEN 1 ELSE 0 END) as has_bool,
            SUM(CASE WHEN byte_value IS NOT NULL THEN 1 ELSE 0 END) as has_byte,
            SUM(CASE WHEN char_value IS NOT NULL THEN 1 ELSE 0 END) as has_char,
            SUM(CASE WHEN short_value IS NOT NULL THEN 1 ELSE 0 END) as has_short,
            SUM(CASE WHEN int_value IS NOT NULL THEN 1 ELSE 0 END) as has_int,
            SUM(CASE WHEN long_value IS NOT NULL THEN 1 ELSE 0 END) as has_long,
            SUM(CASE WHEN float_value IS NOT NULL THEN 1 ELSE 0 END) as has_float,
            SUM(CASE WHEN double_value IS NOT NULL THEN 1 ELSE 0 END) as has_double
          FROM heap_graph_primitive
        """,
        out=Csv('''
          "has_bool","has_byte","has_char","has_short","has_int","has_long","has_float","has_double"
          312,397,160,2532,40751,3078,42,24
        '''))

  def test_art_hprof_decoded_string_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT od.value_string
          FROM heap_graph_object o
          JOIN heap_graph_class c ON o.type_id = c.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE c.name = 'java.lang.String'
            AND od.value_string IS NOT NULL
            AND length(od.value_string) > 1
          ORDER BY od.value_string
          LIMIT 5
        """,
        out=Csv('''
          "value_string"
          "!/"
          "$Proxy"
          ", dst.length="
          "-Infinity"
          "./out/soong/.intermediates/art/tools/ahat/ahat-test-dump/android_common/dex/ahat-test-dump.jar"
        '''))

  def test_art_hprof_primitive_array_no_value_string(self):
    """Primitive array data is stored as blobs, not value_string."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as cnt
          FROM heap_graph_object o
          JOIN heap_graph_class c ON o.type_id = c.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE c.name LIKE '%[]'
            AND od.value_string IS NOT NULL
        """,
        out=Csv('''
          "cnt"
          0
        '''))

  def test_art_hprof_math_double_constants(self):
    """ahat parity: java.lang.Math double constants."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT f.field_name, f.double_value
          FROM heap_graph_primitive f
          JOIN heap_graph_object o ON f.object_id = o.id
          JOIN heap_graph_class c ON o.type_id = c.id
          WHERE c.name = 'java.lang.Class<java.lang.Math>'
            AND f.field_type = 'double'
          ORDER BY f.field_name
        """,
        out=Csv('''
          "field_name","double_value"
          "DEGREES_TO_RADIANS",0.017453
          "E",2.718282
          "PI",3.141593
          "RADIANS_TO_DEGREES",57.295780
          "TAU",6.283185
          "twoToTheDoubleScaleDown",0.000000
          "twoToTheDoubleScaleUp",13407807929942597099574024998205846127479365820592393377723561443721764030073546976801874298166903427690031858186486050853753882811946569946433649006084096.000000
        '''))

  def test_art_hprof_float_special_values(self):
    """Float special values: inf, -inf, NaN (NULL), MAX_VALUE."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT f.field_name, f.float_value
          FROM heap_graph_primitive f
          JOIN heap_graph_object o ON f.object_id = o.id
          JOIN heap_graph_class c ON o.type_id = c.id
          WHERE c.name = 'java.lang.Class<java.lang.Float>'
            AND f.field_type = 'float'
          ORDER BY f.field_name
        """,
        out=Csv('''
          "field_name","float_value"
          "MAX_VALUE",340282346638528859811704183484516925440.000000
          "MIN_NORMAL",0.000000
          "MIN_VALUE",0.000000
          "NEGATIVE_INFINITY",-inf
          "NaN","[NULL]"
          "POSITIVE_INFINITY",inf
        '''))

  def test_art_hprof_boolean_values(self):
    """Boolean field values: java.lang.Boolean.value true and false."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT f.field_name, f.bool_value
          FROM heap_graph_primitive f
          JOIN heap_graph_object o ON f.object_id = o.id
          JOIN heap_graph_class c ON o.type_id = c.id
          WHERE c.name = 'java.lang.Boolean'
            AND f.field_type = 'boolean'
          ORDER BY f.bool_value
        """,
        out=Csv('''
          "field_name","bool_value"
          "java.lang.Boolean.value",0
          "java.lang.Boolean.value",1
        '''))

  def test_art_hprof_int_array_blob(self):
    """ahat test fixture: DumpedStuff.K = int[]{3, 1, 2, 0} stored as blob."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            od.array_element_type,
            od.array_element_count,
            length(__intrinsic_heap_graph_get_array(od.array_data_id)) as blob_len
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name = 'DumpedStuff.K'
        """,
        out=Csv('''
          "array_element_type","array_element_count","blob_len"
          "int",4,16
        '''))

  def test_art_hprof_int_array_json(self):
    """JSON decode of DumpedStuff.K = int[]{3, 1, 2, 0}."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT __intrinsic_heap_graph_get_array_json(
            od.array_data_id, od.array_element_type, od.array_element_count
          ) AS json_data
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name = 'DumpedStuff.K'
        """,
        out=Csv('''
          "json_data"
          "[3,1,2,0]"
        '''))

  def test_art_hprof_byte_array_json(self):
    """JSON decode of DumpedStuff.i = byte[]{0, 1, 2, 3, 4, 5}."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT __intrinsic_heap_graph_get_array_json(
            od.array_data_id, od.array_element_type, od.array_element_count
          ) AS json_data
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name = 'DumpedStuff.i'
        """,
        out=Csv('''
          "json_data"
          "[0,1,2,3,4,5]"
        '''))

  def test_art_hprof_char_array_json(self):
    """JSON decode of DumpedStuff.g = char[]('char thing')."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT __intrinsic_heap_graph_get_array_json(
            od.array_data_id, od.array_element_type, od.array_element_count
          ) AS json_data
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name = 'DumpedStuff.g'
        """,
        out=Csv('''
          "json_data"
          "[99,104,97,114,32,116,104,105,110,103]"
        '''))

  def test_art_hprof_json_null_handling(self):
    """__intrinsic_heap_graph_get_array_json returns NULL for NULL input."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT __intrinsic_heap_graph_get_array_json(
            NULL, 'int', 0
          ) IS NULL as is_null
        """,
        out=Csv('''
          "is_null"
          1
        '''))

  def test_art_hprof_byte_array_blob(self):
    """ahat test fixture: DumpedStuff.i = byte[]{0, 1, 2, 3, 4, 5} as blob."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            od.array_element_type,
            od.array_element_count,
            length(__intrinsic_heap_graph_get_array(od.array_data_id)) as blob_len
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name = 'DumpedStuff.i'
        """,
        out=Csv('''
          "array_element_type","array_element_count","blob_len"
          "byte",6,6
        '''))

  def test_art_hprof_char_array_blob(self):
    """ahat test fixture: DumpedStuff.g = char[]('char thing') as blob."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            od.array_element_type,
            od.array_element_count,
            length(__intrinsic_heap_graph_get_array(od.array_data_id)) as blob_len
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name = 'DumpedStuff.g'
        """,
        out=Csv('''
          "array_element_type","array_element_count","blob_len"
          "char",10,20
        '''))

  def test_art_hprof_hashmap_load_factor(self):
    """ahat parity: HashMap loadFactor = 0.75."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT f.field_name, f.float_value, COUNT() as cnt
          FROM heap_graph_primitive f
          WHERE f.field_name = 'java.util.HashMap.loadFactor'
          GROUP BY f.field_name, f.float_value
        """,
        out=Csv('''
          "field_name","float_value","cnt"
          "java.util.HashMap.loadFactor",0.750000,20
        '''))

  def test_art_hprof_big_array_self_size(self):
    """ahat test fixture: bigArray byte[1000000] has correct self_size."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            o.self_size,
            od.array_element_type,
            od.array_element_count
          FROM heap_graph_object o
          JOIN heap_graph_class c ON o.type_id = c.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE c.name = 'byte[]'
            AND o.self_size > 100000
        """,
        out=Csv('''
          "self_size","array_element_type","array_element_count"
          1000012,"byte",1000000
        '''))

  def test_art_hprof_array_blob_count(self):
    """All primitive arrays have blob metadata columns populated."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as cnt
          FROM heap_graph_object_data
          WHERE array_data_id IS NOT NULL
        """,
        out=Csv('''
          "cnt"
          2037
        '''))

  def test_art_hprof_array_blob_type_distribution(self):
    """Distribution of primitive array types stored as blobs."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT array_element_type, COUNT() as cnt
          FROM heap_graph_object_data
          WHERE array_data_id IS NOT NULL
          GROUP BY array_element_type
          ORDER BY cnt DESC
        """,
        out=Csv('''
          "array_element_type","cnt"
          "byte",1268
          "long",756
          "char",9
          "int",4
        '''))

  def test_art_hprof_array_blob_size_consistency(self):
    """Blob byte length matches element_count * element_size for each type."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as mismatches
          FROM heap_graph_object_data od
          WHERE od.array_data_id IS NOT NULL
            AND length(__intrinsic_heap_graph_get_array(od.array_data_id)) !=
              od.array_element_count * CASE od.array_element_type
                WHEN 'boolean' THEN 1
                WHEN 'byte' THEN 1
                WHEN 'char' THEN 2
                WHEN 'short' THEN 2
                WHEN 'int' THEN 4
                WHEN 'float' THEN 4
                WHEN 'long' THEN 8
                WHEN 'double' THEN 8
              END
        """,
        out=Csv('''
          "mismatches"
          0
        '''))

  def test_art_hprof_no_array_element_rows(self):
    """Primitive array elements are stored as blobs, not per-element rows."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as array_rows
          FROM heap_graph_primitive
          WHERE field_name LIKE '[%'
        """,
        out=Csv('''
          "array_rows"
          0
        '''))

  def test_art_hprof_big_array_blob(self):
    """ahat test fixture: bigArray byte[1000000] stored as 1MB blob."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT
            od.array_element_type,
            od.array_element_count,
            length(__intrinsic_heap_graph_get_array(od.array_data_id)) as blob_len
          FROM heap_graph_object o
          JOIN heap_graph_class c ON o.type_id = c.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE c.name = 'byte[]'
            AND o.self_size > 100000
        """,
        out=Csv('''
          "array_element_type","array_element_count","blob_len"
          "byte",1000000,1000000
        '''))

  def test_art_hprof_get_array_null_handling(self):
    """__intrinsic_heap_graph_get_array returns NULL for NULL input."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT __intrinsic_heap_graph_get_array(NULL) IS NULL as is_null
        """,
        out=Csv('''
          "is_null"
          1
        '''))

  def test_art_hprof_non_array_no_blob(self):
    """Non-array objects have NULL array columns."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as cnt
          FROM heap_graph_object o
          JOIN heap_graph_class c ON o.type_id = c.id
          LEFT JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE c.name = 'DumpedStuff'
            AND (od.array_element_type IS NOT NULL
              OR od.array_element_count IS NOT NULL
              OR od.array_data_id IS NOT NULL)
        """,
        out=Csv('''
          "cnt"
          0
        '''))

  def test_art_hprof_array_data_hash_populated(self):
    """All arrays with blob data have a non-null content hash."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as with_hash
          FROM heap_graph_object_data
          WHERE array_data_id IS NOT NULL
            AND array_data_hash IS NOT NULL
        """,
        out=Csv('''
          "with_hash"
          2037
        '''))

  def test_art_hprof_array_data_hash_null_for_non_arrays(self):
    """Non-array rows have NULL array_data_hash."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as cnt
          FROM heap_graph_object_data
          WHERE array_data_id IS NULL
            AND array_data_hash IS NOT NULL
        """,
        out=Csv('''
          "cnt"
          0
        '''))

  def test_art_hprof_array_data_hash_duplicates(self):
    """Arrays with identical content produce matching hashes."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() as duplicate_hash_groups
          FROM (
            SELECT array_data_hash, COUNT() as cnt
            FROM heap_graph_object_data
            WHERE array_data_hash IS NOT NULL
            GROUP BY array_data_hash
            HAVING cnt > 1
          )
        """,
        out=Csv('''
          "duplicate_hash_groups"
          117
        '''))

  def test_art_hprof_known_array_hash(self):
    """DumpedStuff.K = int[]{3,1,2,0} has a deterministic hash."""
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT od.array_data_hash
          FROM heap_graph_reference r
          JOIN heap_graph_object owner ON r.owner_id = owner.id
          JOIN heap_graph_class oc ON owner.type_id = oc.id
          JOIN heap_graph_object o ON r.owned_id = o.id
          JOIN heap_graph_object_data od ON od.object_id = o.id
          WHERE oc.name = 'DumpedStuff'
            AND r.field_name = 'DumpedStuff.K'
        """,
        out=Csv('''
          "array_data_hash"
          -3333146854241245275
        '''))
