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
                1126
        '''))

  def test_art_hprof_class_examples_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""

          SELECT * FROM heap_graph_class
          ORDER BY name
          LIMIT 10
        """,
        out=Csv('''
                "id","name","deobfuscated_name","location","superclass_id","classloader_id","kind"
                654,"DumpedStuff","[NULL]","[NULL]",1018,0,"[unknown class kind]"
                882,"Main","[NULL]","[NULL]",206,0,"[unknown class kind]"
                1018,"SuperDumpedStuff","[NULL]","[NULL]",206,0,"[unknown class kind]"
                794,"a","[NULL]","[NULL]",206,0,"[unknown class kind]"
                484,"a.a","[NULL]","[NULL]",206,0,"[unknown class kind]"
                1092,"a.b","[NULL]","[NULL]",206,0,"[unknown class kind]"
                4,"a.c","[NULL]","[NULL]",206,0,"[unknown class kind]"
                535,"android.compat.Compatibility","[NULL]","[NULL]",206,0,"[unknown class kind]"
                959,"android.graphics.a","[NULL]","[NULL]",206,0,"[unknown class kind]"
                436,"android.graphics.b","[NULL]","[NULL]",206,0,"[unknown class kind]"
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
          ORDER BY type_id
          LIMIT 10
        """,
        out=Csv('''
                "id","upid","graph_sample_ts","self_size","native_size","reference_set_id","reachable","heap_type","type_id","root_type","root_distance"
                16663,1,1740172787560,0,0,8858,1,"app",0,"STICKY_CLASS",-1
                6693,1,1740172787560,0,0,17046,1,"app",1,"STICKY_CLASS",-1
                4422,1,1740172787560,0,0,19107,1,"app",2,"STICKY_CLASS",-1
                10266,1,1740172787560,0,0,14089,1,"app",3,"STICKY_CLASS",-1
                5038,1,1740172787560,0,0,18550,1,"app",4,"[NULL]",-1
                11779,1,1740172787560,0,0,15702,1,"app",5,"STICKY_CLASS",-1
                538,1,1740172787560,0,0,18319,1,"app",6,"STICKY_CLASS",-1
                5603,1,1740172787560,9,0,17952,1,"app",6,"[NULL]",-1
                12227,1,1740172787560,9,0,12432,1,"app",6,"[NULL]",-1
                12429,1,1740172787560,0,0,12310,1,"app",7,"STICKY_CLASS",-1
        '''))

  def test_art_hprof_reference_count_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT COUNT() FROM heap_graph_reference
        """,
        out=Csv('''
                "COUNT()"
                53770
        '''))

  def test_art_hprof_reference_examples_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('test-dump.hprof'),
        query="""
          SELECT * FROM heap_graph_reference
          ORDER BY field_name
          LIMIT 10
        """,
        out=Csv('''
                "id","reference_set_id","owner_id","owned_id","field_name","field_type_name","deobfuscated_field_name"
                12064,5746,19835,5664,"$VALUES","java.lang.Object","[NULL]"
                47991,21281,5496,4212,"$VALUES","java.lang.Object","[NULL]"
                52499,23130,616,5418,"$VALUES","java.lang.Object","[NULL]"
                5804,2829,22914,20708,"$class$classLoader","java.lang.Object","[NULL]"
                9985,4795,20831,20708,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                10079,4840,20783,20708,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                10146,4871,20753,20708,"$class$classLoader","dalvik.system.PathClassLoader","[NULL]"
                10373,4976,20641,20708,"$class$classLoader","java.lang.Object","[NULL]"
                10439,5007,20608,20708,"$class$classLoader","java.lang.Object","[NULL]"
                10495,5033,20582,20708,"$class$classLoader","java.lang.Object","[NULL]"
        '''))
