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


class MemoryMetrics(TestSuite):
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
          timestamp: 1
          process_tree {
            processes {
              pid: 1
              ppid: 1
              uid: 0
              cmdline: "myprocess"
            }
          }
        }
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
            event {
              timestamp: 150
              pid: 1
              dma_heap_stat {
                inode: 124
                len: 2048
                total_allocated: 4096
              }
            }
            event {
              timestamp: 200
              pid: 1
              dma_heap_stat {
                inode: 123
                len: -1024
                total_allocated: 3072
              }
            }
          }
        }
        """),
        query=Metric('android_dma_heap'),
        out=TextProto(r"""
        android_dma_heap {
            avg_size_bytes: 3072.0
            min_size_bytes: 2048.0
            max_size_bytes: 4096.0
            total_alloc_size_bytes: 3072.0
            total_delta_bytes: 2048
            process_stats {
              process_name: "myprocess"
              delta_bytes: 2048
            }
        }
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

  def test_android_mem_counters(self):
    return DiffTestBlueprint(
        trace=DataPath('memory_counters.pb'),
        query=Metric('android_mem'),
        out=Path('android_mem_counters.out'))

  def test_trace_metadata(self):
    return DiffTestBlueprint(
        trace=DataPath('memory_counters.pb'),
        query=Metric('trace_metadata'),
        out=Path('trace_metadata.out'))

  def test_android_mem_by_priority(self):
    return DiffTestBlueprint(
        trace=Path('android_mem_by_priority.py'),
        query=Metric('android_mem'),
        out=Path('android_mem_by_priority.out'))

  def test_android_mem_lmk(self):
    return DiffTestBlueprint(
        trace=Path('android_systrace_lmk.py'),
        query=Metric('android_lmk'),
        out=TextProto(r"""
        android_lmk {
          total_count: 1
            by_oom_score {
            oom_score_adj: 900
            count: 1
          }
          oom_victim_count: 0
        }
        """))

  def test_android_lmk_oom(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 1000
              ppid: 1
              cmdline: "com.google.android.gm"
            }
            threads {
              tid: 1001
              tgid: 1000
            }
          }
        }
        packet {
          ftrace_events {
            cpu: 4
            event {
              timestamp: 1234
              pid: 4321
              mark_victim {
                pid: 1001
              }
            }
          }
        }
        """),
        query=Metric('android_lmk'),
        out=TextProto(r"""
        android_lmk {
          total_count: 0
          oom_victim_count: 1
        }
        """))

  def test_android_lmk_reason(self):
    return DiffTestBlueprint(
        trace=DataPath('lmk_userspace.pb'),
        query=Metric('android_lmk_reason'),
        # TODO(mayzner): Find a trace that returns results. This is still
        # beneficial though, as at least this metric is run.
        out=TextProto(r"""
        android_lmk_reason {
        }
        """))

  def test_android_mem_delta(self):
    return DiffTestBlueprint(
        trace=Path('android_mem_delta.py'),
        query=Metric('android_mem'),
        out=TextProto(r"""
        android_mem {
          process_metrics {
            process_name: "com.my.pkg"
            total_counters {
              file_rss {
                min: 2000.0
                max: 10000.0
                avg: 6666.666666666667
                delta: 7000.0
              }
            }
          }
        }
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

  def test_android_dma_heap_inode(self):
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
        SELECT
          tt.name,
          tt.utid,
          c.ts,
          CAST(c.value AS INT) AS value,
          args.int_value AS inode
        FROM thread_counter_track tt
          JOIN counter c ON c.track_id = tt.id
          JOIN args USING (arg_set_id)
        WHERE tt.name = 'mem.dma_heap_change' AND args.key = 'inode';
        """,
        out=Csv("""
        "name","utid","ts","value","inode"
        "mem.dma_heap_change",1,100,1024,123
        "mem.dma_heap_change",1,200,-1024,123
        """))
