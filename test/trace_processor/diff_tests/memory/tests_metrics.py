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
