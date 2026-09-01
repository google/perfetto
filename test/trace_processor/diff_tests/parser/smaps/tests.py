#!/usr/bin/env python3
# Copyright (C) 2026 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, DataPath
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class SmapsParser(TestSuite):
  # PackedSmaps in aggregated mode: each row is the aggregate of all VMAs with the same original name.
  def test_packed_smaps_aggregated(self):
    return DiffTestBlueprint(
        trace=DataPath('smaps_aggregated.pftrace'),
        query="""
        SELECT
          process.pid,
          m.path,
          m.aggregate_count,
          m.is_deleted,
          m.size_kb,
          m.rss_kb,
          m.anonymous_kb,
          m.swap_kb
        FROM process_memory_mappings AS m
        JOIN process USING (upid)
        WHERE m.path IN (
          '',
          '/system/bin/surfaceflinger',
          '/system/lib64/libz.so',
          '/dev/ashmem/MessageQueue',
          '[anon:thread signal stack]',
          '[vdso]')
        ORDER BY m.path, m.id;
        """,
        out=Csv("""
        "pid","path","aggregate_count","is_deleted","size_kb","rss_kb","anonymous_kb","swap_kb"
        512,"",220,0,678852,12,12,0
        512,"/dev/ashmem/MessageQueue",8,1,36,28,0,0
        512,"/system/bin/surfaceflinger",4,0,9568,7568,268,52
        512,"/system/lib64/libz.so",3,0,108,100,0,4
        512,"[anon:thread signal stack]",27,0,864,0,0,0
        512,"[vdso]",1,0,4,4,0,0
        """))

  # PackedSmaps in unaggregated mode: each row is a single VMA.
  def test_packed_smaps_unaggregated(self):
    return DiffTestBlueprint(
        trace=DataPath('smaps_unaggregated.pftrace'),
        query="""
        SELECT
          process.pid,
          m.path,
          m.aggregate_count,
          m.is_deleted,
          m.size_kb,
          m.rss_kb,
          m.anonymous_kb,
          m.swap_kb
        FROM process_memory_mappings AS m
        JOIN process USING (upid)
        WHERE m.path IN (
          '/system/bin/surfaceflinger',
          '/system/lib64/libz.so',
          '/dev/ashmem/MessageQueue',
          '[anon:thread signal stack]',
          '[vdso]')
        ORDER BY m.path, m.id;
        """,
        out=Csv("""
        "pid","path","aggregate_count","is_deleted","size_kb","rss_kb","anonymous_kb","swap_kb"
        512,"/dev/ashmem/MessageQueue",1,1,4,4,0,0
        512,"/dev/ashmem/MessageQueue",1,1,4,0,0,0
        512,"/dev/ashmem/MessageQueue",1,1,4,4,0,0
        512,"/dev/ashmem/MessageQueue",1,1,4,4,0,0
        512,"/dev/ashmem/MessageQueue",1,1,8,8,0,0
        512,"/dev/ashmem/MessageQueue",1,1,4,0,0,0
        512,"/dev/ashmem/MessageQueue",1,1,4,4,0,0
        512,"/dev/ashmem/MessageQueue",1,1,4,4,0,0
        512,"/system/bin/surfaceflinger",1,0,2172,720,0,0
        512,"/system/bin/surfaceflinger",1,0,7076,6580,0,0
        512,"/system/bin/surfaceflinger",1,0,276,232,232,44
        512,"/system/bin/surfaceflinger",1,0,44,36,36,8
        512,"/system/lib64/libz.so",1,0,32,32,0,0
        512,"/system/lib64/libz.so",1,0,72,68,0,0
        512,"/system/lib64/libz.so",1,0,4,0,0,4
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[anon:thread signal stack]",1,0,32,0,0,0
        512,"[vdso]",1,0,4,4,0,0
        """))
