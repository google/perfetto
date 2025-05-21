#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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

          SELECT * FROM heap_graph_class
          ORDER BY name, id
          LIMIT 10
        """,
        out=Csv('''
                "id","name","deobfuscated_name","location","superclass_id","classloader_id","kind"
                 827,"DumpedStuff","[NULL]","[NULL]",264,0,"[unknown class kind]"
                 50,"Main","[NULL]","[NULL]",346,0,"[unknown class kind]"
                 264,"SuperDumpedStuff","[NULL]","[NULL]",346,0,"[unknown class kind]"
                 488,"a","[NULL]","[NULL]",346,0,"[unknown class kind]"
                 332,"a.a","[NULL]","[NULL]",346,0,"[unknown class kind]"
                 1096,"a.b","[NULL]","[NULL]",346,0,"[unknown class kind]"
                 365,"a.c","[NULL]","[NULL]",346,0,"[unknown class kind]"
                 656,"android.compat.Compatibility","[NULL]","[NULL]",346,0,"[unknown class kind]"
                 46,"android.graphics.a","[NULL]","[NULL]",346,0,"[unknown class kind]"
                 483,"android.graphics.b","[NULL]","[NULL]",346,0,"[unknown class kind]"
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
          SELECT * FROM heap_graph_object
          ORDER BY type_id, id
          LIMIT 10
        """,
        out=Csv('''
                "id","upid","graph_sample_ts","self_size","native_size","reference_set_id","reachable","heap_type","type_id","root_type","root_distance"
                4239,1,1740172787560,20,0,3879,1,"app",0,"[NULL]",-1
                5646,1,1740172787560,20,0,5168,1,"app",0,"[NULL]",-1
                8855,1,1740172787560,20,0,8142,1,"app",0,"[NULL]",-1
                12738,1,1740172787560,20,0,11741,1,"app",0,"[NULL]",-1
                20760,1,1740172787560,20,0,19092,1,"app",0,"[NULL]",-1
                18135,1,1740172787560,8,0,"[NULL]",1,"app",4,"[NULL]",-1
                17494,1,1740172787560,8,0,16111,1,"app",12,"[NULL]",-1
                2119,1,1740172787560,12,0,1948,1,"app",13,"[NULL]",-1
                5061,1,1740172787560,12,0,4624,1,"app",13,"[NULL]",-1
                5188,1,1740172787560,12,0,4745,1,"app",13,"[NULL]",-1
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
          SELECT * FROM heap_graph_reference
          ORDER BY field_name, id, reference_set_id, owner_id
          LIMIT 10
        """,
        out=Csv('''
                "id","reference_set_id","owner_id","owned_id","field_name","field_type_name","deobfuscated_field_name"
                44126,19995,21757,3576,"$VALUES","libcore.reflect.AnnotationMember$DefaultValues[]","[NULL]"
                48070,21647,23537,4527,"$VALUES","libcore.io.IoTracker$Mode[]","[NULL]"
                52188,23204,25236,5788,"$VALUES","java.io.File$PathStatus[]","[NULL]"
                204,96,101,23897,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                373,172,178,23897,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                956,407,433,23897,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                3222,1465,1597,23897,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                4802,2185,2374,23897,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                12957,5795,6310,23897,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                14820,6545,7117,23897,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
        '''))
