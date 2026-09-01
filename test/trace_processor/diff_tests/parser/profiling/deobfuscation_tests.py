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

from python.generators.diff_tests.testing import Path, DataPath, TextProto
from python.generators.diff_tests.testing import Csv
from python.generators.diff_tests.testing import DiffTestBlueprint
from python.generators.diff_tests.testing import TestSuite


class Deobfuscation(TestSuite):
  # When we cannot infer the package from a mapping
  # ("/system/priv-app/Prebuilt1/Prebuilt1.apk"), we'll fall back to the default
  # package for a process, for perf profiles and heap profiles.
  def test_profile_deobfuscation_default_package(self):
    return DiffTestBlueprint(
        trace=Path('profile_unknown_package.textproto'),
        query="""
        SELECT name, deobfuscated_name
        FROM stack_profile_frame
        ORDER BY 1, 2
        """,
        out=Csv("""
        "name","deobfuscated_name"
        "hwe.a","com.google.classfour.foo"
        "hwe.a","com.google.classtwo.foo"
        "hye.a","com.google.classone.foo"
        "hye.a","com.google.classthree.foo"
        """))

  def test_perf_data_symbols_deobfuscation(self):
    return DiffTestBlueprint(
        trace=DataPath('perf-data-deobfuscated.zip'),
        query="""
        SELECT count() AS cnt
        FROM stack_profile_frame
        WHERE deobfuscated_name IS NOT NULL
        """,
        out=Csv("""
        "cnt"
        839
        """))

  def test_art_oome_stack_sample_deobfuscation(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          process_tree {
            processes {
              pid: 1
              ppid: 0
              cmdline: "init"
              uid: 0
            }
            processes {
              pid: 12345
              ppid: 1
              cmdline: "com.example.oometest"
              uid: 10155
            }
          }
        }
        packet {
          packages_list {
            packages {
              name: "com.example.oometest"
              uid: 10155
            }
          }
        }
        packet {
          timestamp: 1234567890123456
          trusted_packet_sequence_id: 1
          art_process_metadata {
            pid: 12345
            uid: 10155
            process_name: "com.example.oometest"
            oom_allocation_size: 1048576
            oom_total_bytes_free: 512000
            oom_free_bytes_until_oom: 204800
            oom_error_msg: "Failed to allocate 1048576 bytes"
            oom_thread_java_stack {
              frames {
                method_name: "hye.a"
                source_file: "MainActivity.java"
                line_number: 45
              }
              frames {
                method_name: "hwe.a"
                source_file: "MainActivity.java"
                line_number: 42
              }
            }
          }
        }
        packet {
          deobfuscation_mapping {
            package_name: "com.example.oometest"
            obfuscated_classes {
              obfuscated_name: "hye"
              deobfuscated_name: "com.google.classone"
              obfuscated_methods {
                obfuscated_name: "a"
                deobfuscated_name: "foo"
              }
            }
            obfuscated_classes {
              obfuscated_name: "hwe"
              deobfuscated_name: "com.google.classtwo"
              obfuscated_methods {
                obfuscated_name: "a"
                deobfuscated_name: "bar"
              }
            }
          }
        }
        """),
        query="""
        SELECT name, deobfuscated_name
        FROM stack_profile_frame
        ORDER BY name;
        """,
        out=Csv("""
        "name","deobfuscated_name"
        "hwe.a","com.google.classtwo.bar"
        "hye.a","com.google.classone.foo"
        """))
