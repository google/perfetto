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


class TranslatedArgs(TestSuite):

  def test_java_class_name_arg(self):
    return DiffTestBlueprint(
        trace=Path('java_class_name_arg.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('java_class_name_arg.out'))

  def test_chrome_histogram(self):
    return DiffTestBlueprint(
        trace=Path('chrome_histogram.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('chrome_histogram.out'))

  def test_chrome_user_event(self):
    return DiffTestBlueprint(
        trace=Path('chrome_user_event.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('chrome_user_event.out'))

  def test_chrome_performance_mark(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
        packet {
          translation_table {
            chrome_performance_mark {
              site_hash_to_name { key: 10 value: "site1" }
              mark_hash_to_name { key: 20 value: "mark2" }
            }
          }
        }
        packet {
          timestamp: 0
          trusted_packet_sequence_id: 1
          track_descriptor {
            uuid: 12345
            thread {
              pid: 123
              tid: 345
            }
            parent_uuid: 0
            chrome_thread {
              thread_type: THREAD_POOL_FG_WORKER
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 1
          track_event {
            categories: "cat1"
            track_uuid: 12345
            type: 1
            name: "slice1"
            [perfetto.protos.ChromeTrackEvent.chrome_hashed_performance_mark] {
              site_hash: 10
              mark_hash: 20
            }
          }
        }
        packet {
          trusted_packet_sequence_id: 1
          timestamp: 6000
          track_event {
            track_uuid: 12345
            categories: "cat1"
            name: "slice1"
            type: 2
          }
        }
        """),
        query=Path('chrome_args_test.sql'),
        out=Path('chrome_performance_mark.out'))

  def test_slice_name(self):
    return DiffTestBlueprint(
        trace=Path('slice_name.textproto'),
        query="""
        SELECT name FROM slice ORDER BY name;
        """,
        out=Csv("""
        "name"
        "mapped_name1"
        "mapped_name2"
        "raw_name3"
        "slice_begin"
        """))

  def test_slice_name_2(self):
    return DiffTestBlueprint(
        trace=Path('slice_name_negative_timestamp.textproto'),
        query="""
        SELECT name FROM slice ORDER BY name;
        """,
        out=Csv("""
        "name"
        "mapped_name1"
        "mapped_name2"
        "raw_name3"
        "slice_begin"
        """))

  def test_process_track_name(self):
    return DiffTestBlueprint(
        trace=Path('process_track_name.textproto'),
        query="""
        SELECT
          name
        FROM track
        WHERE
          name IS NOT NULL
          AND type in ('process_track', 'process_counter_track')
        ORDER BY name;
        """,
        out=Csv("""
        "name"
        "explicitly_renamed"
        "implicitly_renamed"
        "renamed_counter"
        """))

  def test_native_symbol_arg(self):
    return DiffTestBlueprint(
        trace=Path('native_symbol_arg.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('native_symbol_arg.out'))

  def test_native_symbol_arg_2(self):
    return DiffTestBlueprint(
        trace=Path('native_symbol_arg_incomplete.textproto'),
        query=Path('chrome_args_test.sql'),
        out=Path('native_symbol_arg.out'))
