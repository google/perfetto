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
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class ProfilingHeapProfiling(TestSuite):

  def test_heap_profile_jit(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_jit.textproto'),
        query="""
        SELECT name, mapping, rel_pc FROM stack_profile_frame ORDER BY name;
        """,
        out=Csv("""
        "name","mapping","rel_pc"
        "java_frame_1",0,4096
        "java_frame_2",0,4096
        """))

  def test_heap_profile_deobfuscate(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_deobfuscate.textproto'),
        query=Path('heap_profile_deobfuscate_test.sql'),
        out=Csv("""
        "deobfuscated_name","mapping","rel_pc"
        "Bar.function1",0,4096
        """))

  def test_heap_profile_deobfuscate_2(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_deobfuscate_memfd.textproto'),
        query=Path('heap_profile_deobfuscate_test.sql'),
        out=Csv("""
        "deobfuscated_name","mapping","rel_pc"
        "Bar.function1",0,4096
        """))

  def test_heap_profile_dump_max_legacy(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max_legacy.textproto'),
        query="""
        SELECT * FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","type","ts","upid","heap_name","callsite_id","count","size"
        0,"heap_profile_allocation",-10,2,"unknown",2,0,1000
        1,"heap_profile_allocation",-10,2,"unknown",3,0,90
        """))

  def test_heap_profile_dump_max(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_dump_max.textproto'),
        query="""
        SELECT * FROM heap_profile_allocation;
        """,
        out=Csv("""
        "id","type","ts","upid","heap_name","callsite_id","count","size"
        0,"heap_profile_allocation",-10,2,"unknown",2,6,1000
        1,"heap_profile_allocation",-10,2,"unknown",3,1,90
        """))
