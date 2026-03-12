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
