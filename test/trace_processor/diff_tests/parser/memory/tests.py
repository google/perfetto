#!/usr/bin/env python3
# Copyright (C) 2023 The Android Open Source Project
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

from python.generators.diff_tests.testing import Csv, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class MemoryParser(TestSuite):
  # cma alloc
  def test_cma(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              release: "5.10.0"
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 4
            event {
              timestamp: 74288080958099
              pid: 537
              cma_alloc_start {
                align: 4
                count: 6592
                name: "farawimg"
              }
            }
            event {
              timestamp: 74288191109751
              pid: 537
              cma_alloc_info {
                align: 4
                count: 6592
                err_iso: 0
                err_mig: 0
                err_test: 0
                name: "farawimg"
                nr_mapped: 832596
                nr_migrated: 6365
                nr_reclaimed: 7
                pfn: 10365824
              }
            }
          }
        }
        """),
        query="""
        SELECT ts, dur, name FROM slice WHERE name = 'mm_cma_alloc';
        """,
        out=Csv("""
        "ts","dur","name"
        74288080958099,110151652,"mm_cma_alloc"
        """))

  def test_android_dma_buffer_tracks(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100
              pid: 1
              dma_heap_stat {
                inode: 123
                len: 1024
                total_allocated: 2048
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 200
              pid: 1
              dma_heap_stat {
                inode: 123
                len: -1024
                total_allocated: 1024
              }
            }
          }
        }
        """),
        query="""
        SELECT track.name, slice.ts, slice.dur, slice.name
        FROM slice JOIN track ON slice.track_id = track.id
        WHERE track.name = 'mem.dma_buffer';
        """,
        out=Csv("""
        "name","ts","dur","name"
        "mem.dma_buffer",100,100,"1 kB"
        """))

  def test_cma_alloc_finish(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          system_info {
            utsname {
              sysname: "Linux"
              release: "6.1.0"
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 2
            event {
              timestamp: 80000000000000
              pid: 123
              cma_alloc_start {
                align: 8
                count: 1024
                name: "test_cma"
              }
            }
            event {
              timestamp: 80000000000050
              pid: 123
              mm_alloc_contig_migrate_range_info {
                start: 1234
                end: 2258
                migratetype: 64
                nr_migrated: 10
                nr_reclaimed: 1000
                nr_mapped: 99
              }
            }
            event {
              timestamp: 80000000000100
              pid: 123
              cma_alloc_finish {
                align: 8
                count: 1024
                name: "test_cma"
                page: 2000
                pfn: 2048
              }
            }
          }
        }
        """),
        query="""
        SELECT ts, dur, name,
        EXTRACT_ARG(arg_set_id, 'cma_nr_migrated') AS cma_nr_migrated,
        EXTRACT_ARG(arg_set_id, 'cma_nr_reclaimed') AS cma_nr_reclaimed,
        EXTRACT_ARG(arg_set_id, 'cma_nr_mapped') AS cma_nr_mapped
        FROM slice WHERE name = 'mm_cma_alloc';
        """,
        out=Csv("""
        "ts","dur","name","cma_nr_migrated","cma_nr_reclaimed","cma_nr_mapped"
        80000000000000,100,"mm_cma_alloc",10,1000,99
        """))

  # linux.cgroup memcg values: one track per cgroup and value, for processes
  # (pid level), uids and the top-level groups, with distinct track types for
  # the sizes, the limits and the event counts. The polls after the first only
  # have some of the values, as with MemcgConfig.cache_ttl_ms.
  # A limit set to "max" is UINT64_MAX in the trace and -1 in the counter.
  # The process tracks also have the group and the uid of the cgroup.
  def test_memcg_stats(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          timestamp: 1000
          memcg_stats {
            cgroup {
              android_group: GROUP_APPS
              uid: 10001
              pid: 42
              current: 100
              high: 18446744073709551615
              swap_current: 0
              events_local { oom_kill: 1 }
              stat { anon: 10 pgsteal: 5 }
            }
            cgroup {
              android_group: GROUP_APPS
              uid: 10001
              current: 300
              high: 18446744073709551615
            }
            cgroup { android_group: GROUP_SYSTEM uid: 0 current: 50 }
            cgroup { android_group: GROUP_SYSTEM current: 1000 }
          }
        }
        packet {
          timestamp: 2000
          memcg_stats {
            cgroup {
              android_group: GROUP_APPS
              uid: 10001
              pid: 42
              current: 150
              high: 5000
            }
          }
        }
        packet {
          timestamp: 3000
          memcg_stats {
            cgroup {
              android_group: GROUP_APPS
              uid: 10001
              pid: 42
              current: 150
              high: 18446744073709551615
              stat { anon: 10 pgsteal: 7 }
            }
            cgroup { android_group: GROUP_APPS uid: 10001 current: 300 }
            cgroup { android_group: GROUP_SYSTEM uid: 0 current: 50 }
            cgroup { android_group: GROUP_SYSTEM current: 1000 }
          }
        }
        """),
        query="""
        SELECT
          t.type,
          t.name,
          t.unit,
          EXTRACT_ARG(t.dimension_arg_set_id, 'memcg_group') AS memcg_group,
          EXTRACT_ARG(t.dimension_arg_set_id, 'uid') AS uid,
          p.pid,
          c.ts,
          c.value
        FROM counter c
        JOIN counter_track t ON c.track_id = t.id
        LEFT JOIN process p
          ON p.upid = EXTRACT_ARG(t.dimension_arg_set_id, 'upid')
        WHERE t.name GLOB 'memcg.*'
        ORDER BY t.name, c.ts;
        """,
        out=Csv("""
        "type","name","unit","memcg_group","uid","pid","ts","value"
        "memcg_uid","memcg.apps.uid_10001.current","bytes","apps",10001,"[NULL]",1000,300.000000
        "memcg_uid","memcg.apps.uid_10001.current","bytes","apps",10001,"[NULL]",3000,300.000000
        "memcg_uid_limit","memcg.apps.uid_10001.high","bytes","apps",10001,"[NULL]",1000,-1.000000
        "memcg_process","memcg.current","bytes","apps",10001,42,1000,100.000000
        "memcg_process","memcg.current","bytes","apps",10001,42,2000,150.000000
        "memcg_process","memcg.current","bytes","apps",10001,42,3000,150.000000
        "memcg_process_event","memcg.events.local.oom_kill","[NULL]","apps",10001,42,1000,1.000000
        "memcg_process_limit","memcg.high","bytes","apps",10001,42,1000,-1.000000
        "memcg_process_limit","memcg.high","bytes","apps",10001,42,2000,5000.000000
        "memcg_process_limit","memcg.high","bytes","apps",10001,42,3000,-1.000000
        "memcg_process","memcg.stat.anon","bytes","apps",10001,42,1000,10.000000
        "memcg_process","memcg.stat.anon","bytes","apps",10001,42,3000,10.000000
        "memcg_process_event","memcg.stat.pgsteal","[NULL]","apps",10001,42,1000,5.000000
        "memcg_process_event","memcg.stat.pgsteal","[NULL]","apps",10001,42,3000,7.000000
        "memcg_process","memcg.swap.current","bytes","apps",10001,42,1000,0.000000
        "memcg_group","memcg.system.current","bytes","system","[NULL]","[NULL]",1000,1000.000000
        "memcg_group","memcg.system.current","bytes","system","[NULL]","[NULL]",3000,1000.000000
        "memcg_uid","memcg.system.uid_0.current","bytes","system",0,"[NULL]",1000,50.000000
        "memcg_uid","memcg.system.uid_0.current","bytes","system",0,"[NULL]",3000,50.000000
        """))
