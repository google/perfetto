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
