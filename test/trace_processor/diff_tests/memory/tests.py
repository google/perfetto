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


class Memory(TestSuite):
  # Contains test for Android memory metrics. ION metric
  def test_android_ion(self):
    return DiffTestBlueprint(
        trace=Path('android_ion.py'),
        query=Metric('android_ion'),
        out=TextProto(r"""
        android_ion {
          buffer {
            name: "adsp"
            avg_size_bytes: 1000.0
            min_size_bytes: 1000.0
            max_size_bytes: 1100.0
            total_alloc_size_bytes: 1100.0
          }
          buffer {
            name: "system"
            avg_size_bytes: 1497.4874371859296
            min_size_bytes: 1000.0
            max_size_bytes: 2000.0
            total_alloc_size_bytes: 2000.0
          }
        }
        """))

  def test_android_ion_stat(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100
              pid: 1
              ion_stat {
                buffer_id: 123
                len: 1000
                total_allocated: 2000
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
              ion_stat {
                buffer_id: 123
                len: -1000
                total_allocated: 1000
              }
            }
          }
        }
        """),
        query=Metric('android_ion'),
        out=TextProto(r"""
        android_ion {
          buffer {
            name: "all"
            avg_size_bytes: 2000.0
            min_size_bytes: 1000.0
            max_size_bytes: 2000.0
            total_alloc_size_bytes: 1000.0
          }
        }
        """))

  # DMA-BUF heap Metric
  def test_android_dma_heap_stat(self):
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
        query=Metric('android_dma_heap'),
        out=TextProto(r"""
        android_dma_heap {
            avg_size_bytes: 2048.0
            min_size_bytes: 1024.0
            max_size_bytes: 2048.0
            total_alloc_size_bytes: 1024.0
        }
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

  # fastrpc metric
  def test_android_fastrpc_dma_stat(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 0
            event {
              timestamp: 100
              pid: 1
              fastrpc_dma_stat {
                cid: 1
                len: 1000
                total_allocated: 2000
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
              fastrpc_dma_stat {
                cid: 1
                len: -1000
                total_allocated: 1000
              }
            }
          }
        }
        """),
        query=Metric('android_fastrpc'),
        out=TextProto(r"""
        android_fastrpc {
          subsystem {
            name: "MDSP"
            avg_size_bytes: 2000.0
            min_size_bytes: 1000.0
            max_size_bytes: 2000.0
            total_alloc_size_bytes: 1000.0
          }
        }
        """))

  # shrink slab
  def test_shrink_slab(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          ftrace_events {
            cpu: 7
            event {
              timestamp: 36448185787847
              pid: 156
              mm_shrink_slab_start {
                cache_items: 1
                delta: 0
                gfp_flags: 3264
                nr_objects_to_shrink: 0
                shr: 18446743882920355600
                shrink: 90
                total_scan: 0
                nid: 0
                priority: 12
              }
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 7
            event {
              timestamp: 36448185788539
              pid: 156
              mm_shrink_slab_end {
                new_scan: 0
                retval: 0
                shr: 18446743882920355600
                shrink: 90
                total_scan: 0
                unused_scan: 0
                nid: 0
              }
            }
          }
        }
        """),
        query="""
        SELECT ts, dur, name FROM slice WHERE name = 'mm_vmscan_shrink_slab';
        """,
        out=Csv("""
        "ts","dur","name"
        36448185787847,692,"mm_vmscan_shrink_slab"
        """))

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
