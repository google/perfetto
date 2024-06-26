#!/usr/bin/env python3
# Copyright (C) 2024 The Android Open Source Project
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

from python.generators.diff_tests.testing import Path, DataPath, Metric, Systrace
from python.generators.diff_tests.testing import Csv, Json, TextProto, BinaryProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite
from python.generators.diff_tests.testing import PrintProfileProto


class AndroidMemory(TestSuite):

  def test_memory_oom_score_with_rss_and_swap_per_process(self):
    return DiffTestBlueprint(
        trace=DataPath('sched_wakeup_trace.atr'),
        query="""
        INCLUDE PERFETTO MODULE android.memory.process;
        SELECT *
        FROM memory_oom_score_with_rss_and_swap_per_process
        WHERE oom_adj_reason IS NOT NULL
        ORDER BY ts
        LIMIT 10;
      """,
        out=Csv("""
        "ts","dur","score","bucket","upid","process_name","pid","oom_adj_id","oom_adj_ts","oom_adj_dur","oom_adj_track_id","oom_adj_thread_name","oom_adj_reason","oom_adj_trigger","anon_rss","file_rss","shmem_rss","rss","swap","anon_rss_and_swap","rss_and_swap"
        1737065264829,701108081,925,"cached",269,"com.android.providers.calendar",1937,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",49229824,57495552,835584,107560960,0,49229824,107560960
        1737066678827,2934486383,935,"cached",287,"com.android.imsserviceentitlement",2397,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",48881664,57081856,831488,106795008,0,48881664,106795008
        1737066873002,2934292208,945,"cached",292,"com.android.carrierconfig",2593,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",48586752,49872896,823296,99282944,0,48586752,99282944
        1737067058812,2934106398,955,"cached",288,"com.android.messaging",2416,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",54956032,71417856,843776,127217664,0,54956032,127217664
        1737067246975,699224817,955,"cached",267,"android.process.acore",1866,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",52498432,72048640,856064,125403136,0,52498432,125403136
        1737068421919,2932743291,965,"cached",273,"com.android.shell",2079,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",48738304,52056064,823296,101617664,0,48738304,101617664
        1737068599673,970398,965,"cached",271,"android.process.media",2003,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",49917952,60444672,839680,111202304,0,49917952,111202304
        1737068933602,2932231608,975,"cached",286,"com.android.gallery3d",2371,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",49561600,54521856,831488,104914944,0,49561600,104914944
        1737069091010,682459310,975,"cached",289,"com.android.packageinstaller",2480,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",49364992,52539392,827392,102731776,0,49364992,102731776
        1737069240534,489635,985,"cached",268,"com.android.managedprovisioning",1868,332,1737064421516,29484835,1217,"binder:642_1","processEnd","IActivityManager#1598246212",50683904,53985280,815104,105484288,0,50683904,105484288
         """))

  def test_memory_dmabuf(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 1
              pid: 3000
              dma_heap_stat {
                inode: 13583
                len: 3000
                total_allocated: 3000
              }
            }
            event {
              timestamp: 2
              pid: 3000
              dma_heap_stat {
                inode: 13583
                len: -3000
                total_allocated: 0
              }
            }
            event {
              timestamp: 4144791776152
              pid: 9403
              binder_transaction {
                debug_id: 3052940
                target_node: 256
                to_proc: 572
                to_thread: 0
                reply: 0
                code: 1
                flags: 16
              }
            }
            event {
              timestamp: 4144791793486
              pid: 591
              binder_transaction_received {
                debug_id: 3052940
              }
            }
            event {
              timestamp: 4144792258492
              pid: 591
              dma_heap_stat {
                inode: 13583
                len: 10399744
                total_allocated: 254873600
              }
            }
            event {
              timestamp: 4144792517566
              pid: 591
              binder_transaction {
                debug_id: 3052950
                target_node: 0
                to_proc: 2051
                to_thread: 9403
                reply: 1
                code: 0
                flags: 0
              }
            }
            event {
              timestamp: 4144792572498
              pid: 9403
              binder_transaction_received {
                debug_id: 3052950
              }
            }
            event {
              timestamp: 4145263509021
              pid: 613
              dma_heap_stat {
                inode: 13583
                len: -10399744
                total_allocated: 390160384
              }
            }
          }
        }"""),
        query="""
        INCLUDE PERFETTO MODULE android.memory.dmabuf;
        SELECT * FROM android_dmabuf_allocs;
        """,
        out=Csv("""
        "ts","buf_size","inode","utid","tid","thread_name","upid","pid","process_name"
        1,3000,13583,1,3000,"[NULL]","[NULL]","[NULL]","[NULL]"
        2,-3000,13583,1,3000,"[NULL]","[NULL]","[NULL]","[NULL]"
        4144792258492,10399744,13583,3,591,"[NULL]","[NULL]","[NULL]","[NULL]"
        4145263509021,-10399744,13583,3,591,"[NULL]","[NULL]","[NULL]","[NULL]"
         """))