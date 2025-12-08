#!/usr/bin/env python3
# Copyright (C) 2025 The Android Open Source Project
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


class Stacks(TestSuite):

  def test_symbolization_candidates_heap_profile(self):
    return DiffTestBlueprint(
        trace=TextProto("""
        packet {
          clock_snapshot {
            clocks: {
              clock_id: 6 # BOOTTIME
              timestamp: 0
            }
            clocks: {
              clock_id: 4 # MONOTONIC_COARSE
              timestamp: 10
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 999
          timestamp: 11
          profile_packet {
            strings {
              iid: 1
              str: "f1"
            }
            strings {
              iid: 2
              str: "f2"
            }
            strings {
              iid: 4
              str: "libmonochrome_64.so"
            }
            strings {
              iid: 5
              str: "\x7f\x07\x15\xc2\x86\xf8\xb1\x6c\x10\xe4\xad\x34\x9c\xda\x3b\x9b\x56\xc7\xa7\x73"
            }
            frames {
              iid: 1
              function_name_id: 1
              mapping_id: 1
              rel_pc: 0x1000
            }
            frames {
              iid: 2
              function_name_id: 2
              mapping_id: 1
              rel_pc: 0x2000
            }
            callstacks {
              iid: 1
              frame_ids: 1
              frame_ids: 2
            }
            mappings {
              iid: 1
              path_string_ids: 4
              build_id: 5
            }
            process_dumps {
              pid: 2
              samples {
                callstack_id: 1
                self_allocated: 2000
                self_freed: 1000
                alloc_count: 2
                free_count: 1
              }
            }
            process_dumps {
              pid: 3
              samples {
                callstack_id: 1
                self_allocated: 3000
                self_freed: 2000
                alloc_count: 3
                free_count: 2
              }
            }
            index: 0
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE stacks.symbolization_candidates;
        SELECT * FROM _stacks_symbolization_candidates ORDER BY upid, build_id, rel_pc;
        """,
        out=Csv("""
        "upid","module","build_id","rel_pc","breakpad_module_id"
        1,"/libmonochrome_64.so","7f0715c382c286c3b8c2b16c10c3a4c2ad34c29cc39a3bc29b56c387c2a773",4096,"c315077fc282c386b8c2b16c10c3a4c20"
        1,"/libmonochrome_64.so","7f0715c382c286c3b8c2b16c10c3a4c2ad34c29cc39a3bc29b56c387c2a773",8192,"c315077fc282c386b8c2b16c10c3a4c20"
        2,"/libmonochrome_64.so","7f0715c382c286c3b8c2b16c10c3a4c2ad34c29cc39a3bc29b56c387c2a773",4096,"c315077fc282c386b8c2b16c10c3a4c20"
        2,"/libmonochrome_64.so","7f0715c382c286c3b8c2b16c10c3a4c2ad34c29cc39a3bc29b56c387c2a773",8192,"c315077fc282c386b8c2b16c10c3a4c20"
        """))

  def test_symbolization_candidates_perf_profile(self):
    return DiffTestBlueprint(
        trace=TextProto("""
        packet {
          clock_snapshot {
            clocks: {
              clock_id: 6 # BOOTTIME
              timestamp: 0
            }
            clocks: {
              clock_id: 4 # MONOTONIC_COARSE
              timestamp: 10
            }
          }
        }
        packet {
          timestamp: 6
          # These two are necessary for interning to work. Otherwise the packet is
          # silently dropped.
          trusted_packet_sequence_id: 1
          incremental_state_cleared: true
          interned_data {
            build_ids {
              iid: 31313
              str: "build-id"
            }
            function_names {
              iid: 1
              str: "android::fn1"
            }
            mapping_paths {
              iid: 1
              str: "libc.so"
            }
            mappings {
              iid: 1
              build_id: 31313
              path_string_ids: 1
            }
            frames {
              iid: 1
              function_name_id: 1
              mapping_id: 1
              rel_pc: 123456
            }
            callstacks {
              iid: 1
              frame_ids: 1
            }
          }
          perf_sample {
            cpu: 0
            cpu_mode: MODE_GUEST_USER
            pid: 123
            tid: 1234
            callstack_iid: 1
          }
        }
        """),
        query="""
        INCLUDE PERFETTO MODULE stacks.symbolization_candidates;
        SELECT * FROM _stacks_symbolization_candidates ORDER BY upid, build_id, rel_pc;
        """,
        out=Csv("""
        "upid","module","build_id","rel_pc","breakpad_module_id"
        1,"/libc.so","6275696c642d6964",123456,"[NULL]"
        """))
