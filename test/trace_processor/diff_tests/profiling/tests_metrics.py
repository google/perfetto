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


class ProfilingMetrics(TestSuite):

  def test_unsymbolized_frames(self):
    return DiffTestBlueprint(
        trace=Path('heap_profile_no_symbols.textproto'),
        query=Metric('unsymbolized_frames'),
        out=TextProto(r"""
        unsymbolized_frames {
          frames {
            module: "/liblib.so"
            build_id: "6275696c642d6964"
            address: 4096
            google_lookup_id: "6275696c642d6964"
          }
          frames {
            module: "/liblib.so"
            build_id: "6275696c642d6964"
            address: 8192
            google_lookup_id: "6275696c642d6964"
          }
          frames {
            module: "/libmonochrome_64.so"
            build_id: "7f0715c286f8b16c10e4ad349cda3b9b56c7a773"
            address: 4096
            google_lookup_id: "c215077ff8866cb110e4ad349cda3b9b0"
          }
          frames {
            module: "/libmonochrome_64.so"
            build_id: "7f0715c286f8b16c10e4ad349cda3b9b56c7a773"
            address: 8192
            google_lookup_id: "c215077ff8866cb110e4ad349cda3b9b0"
          }
        }
        """))

  def test_simpleperf_event(self):
    return DiffTestBlueprint(
        trace=Path('simpleperf_event.py'),
        query=Metric('android_simpleperf'),
        out=Path('simpleperf_event.out'))

  def test_java_heap_stats(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Metric('java_heap_stats'),
        out=TextProto(r"""
        java_heap_stats {
          instance_stats {
            upid: 2
            process {
              name: "system_server"
              uid: 1000
              pid: 2
            }
            samples {
              ts: 10
              heap_size: 1760
              heap_native_size: 0
              reachable_heap_size: 352
              reachable_heap_native_size: 0
              obj_count: 6
              reachable_obj_count: 3
              anon_rss_and_swap_size: 4096000
              oom_score_adj: 0
              roots {
                root_type: "ROOT_JAVA_FRAME"
                type_name: "DeobfuscatedA[]"
                obj_count: 1
              }
              roots {
                root_type: "ROOT_JAVA_FRAME"
                type_name: "FactoryProducerDelegateImplActor"
                obj_count: 1
              }
            }
          }
        }
        """))

  def test_heap_stats_closest_proc(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph_closest_proc.textproto'),
        query=Metric('java_heap_stats'),
        out=Path('heap_stats_closest_proc.out'))

  def test_java_heap_histogram(self):
    return DiffTestBlueprint(
        trace=Path('heap_graph.textproto'),
        query=Metric('java_heap_histogram'),
        out=Path('java_heap_histogram.out'))
