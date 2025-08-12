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
          1740172787560,1000000,0,1,"app","[NULL]",-1
          1740172787560,16384,0,1,"app","[NULL]",-1
          1740172787560,8192,0,1,"app","[NULL]",-1
          1740172787560,6576,0,1,"app","[NULL]",-1
          1740172787560,2800,0,1,"app","[NULL]",-1
          1740172787560,2388,0,1,"app","STICKY_CLASS",-1
          1740172787560,2048,0,1,"app","[NULL]",-1
          1740172787560,2048,0,1,"app","[NULL]",-1
          1740172787560,2048,0,1,"app","[NULL]",-1
          1740172787560,2048,0,1,"app","[NULL]",-1
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
          "$VALUES","java.io.File$PathStatus[]","java.lang.Class<java.io.File$PathStatus>","java.io.File$PathStatus[]"
          "$VALUES","libcore.io.IoTracker$Mode[]","java.lang.Class<libcore.io.IoTracker$Mode>","libcore.io.IoTracker$Mode[]"
          "$VALUES","libcore.reflect.AnnotationMember$DefaultValues[]","java.lang.Class<libcore.reflect.AnnotationMember$DefaultValues>","libcore.reflect.AnnotationMember$DefaultValues[]"
          "$class$classLoader","dalvik.system.PathClassLoader","java.lang.Class<DumpedStuff>","dalvik.system.PathClassLoader"
          "$class$classLoader","dalvik.system.PathClassLoader","java.lang.Class<Main>","dalvik.system.PathClassLoader"
          "$class$classLoader","dalvik.system.PathClassLoader","java.lang.Class<SuperDumpedStuff>","dalvik.system.PathClassLoader"
          "$class$classLoader","dalvik.system.PathClassLoader","java.lang.Class<a.a>","dalvik.system.PathClassLoader"
          "$class$classLoader","dalvik.system.PathClassLoader","java.lang.Class<a.b>","dalvik.system.PathClassLoader"
          "$class$classLoader","dalvik.system.PathClassLoader","java.lang.Class<a.c>","dalvik.system.PathClassLoader"
          "$class$classLoader","dalvik.system.PathClassLoader","java.lang.Class<a>","dalvik.system.PathClassLoader"
        '''))
