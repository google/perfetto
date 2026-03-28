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

from python.generators.diff_tests.testing import Path, DataPath, Metric
from python.generators.diff_tests.testing import Csv, Json, TextProto
from python.generators.diff_tests.testing import DiffTestBlueprint, TraceInjector
from python.generators.diff_tests.testing import TestSuite


class GraphicsGpuTrace(TestSuite):

  def test_gpu_counters(self):
    return DiffTestBlueprint(
        trace=Path('gpu_counters.py'),
        query="""
        SELECT "ts", "value", "name", "gpu_id", "description", "unit"
        FROM counter
        JOIN gpu_counter_track
          ON counter.track_id = gpu_counter_track.id
        ORDER BY "ts";
        """,
        out=Csv("""
        "ts","value","name","gpu_id","description","unit"
        11,10.000000,"Vertex / Second",0,"Number of vertices per second","Vertex/s"
        12,14.000000,"Fragment / Second",0,"Number of fragments per second","Pixel/s"
        14,9.000000,"Triangle Acceleration",1,"Number of triangles per ms-ms","Triangle/ms:ms"
        15,0.000000,"Bytes Only",0,"Counter with NONE denominator","B"
        16,0.000000,"Frequency",0,"Counter with numerator only","Hz"
        21,15.500000,"Vertex / Second",0,"Number of vertices per second","Vertex/s"
        22,21.000000,"Fragment / Second",0,"Number of fragments per second","Pixel/s"
        24,7.000000,"Triangle Acceleration",1,"Number of triangles per ms-ms","Triangle/ms:ms"
        25,0.000000,"Bytes Only",0,"Counter with NONE denominator","B"
        26,0.000000,"Frequency",0,"Counter with numerator only","Hz"
        31,0.000000,"Vertex / Second",0,"Number of vertices per second","Vertex/s"
        32,0.000000,"Fragment / Second",0,"Number of fragments per second","Pixel/s"
        34,0.000000,"Triangle Acceleration",1,"Number of triangles per ms-ms","Triangle/ms:ms"
        """))

  def test_gpu_table(self):
    return DiffTestBlueprint(
        trace=Path('gpu_counters.py'),
        query="""
        SELECT gpu, machine_id
        FROM gpu
        ORDER BY gpu;
        """,
        out=Csv("""
        "gpu","machine_id"
        0,0
        1,0
        """))

  def test_gpu_table_machine_id(self):
    return DiffTestBlueprint(
        trace=Path('gpu_counters.py'),
        trace_modifier=TraceInjector(['gpu_counter_event'],
                                     {'machine_id': 1001}),
        query="""
        SELECT gpu, machine_id
        FROM gpu
        ORDER BY gpu;
        """,
        out=Csv("""
        "gpu","machine_id"
        0,1
        1,1
        """))

  def test_gpu_counter_specs(self):
    return DiffTestBlueprint(
        trace=Path('gpu_counter_specs.textproto'),
        query="""
        SELECT group_id, c.name, c.description, unit
        FROM gpu_counter_group AS g
        JOIN gpu_counter_track AS c
          ON g.track_id = c.id;
        """,
        out=Csv("""
        "group_id","name","description","unit"
        0,"GPU Frequency","clock speed","/s"
        3,"Fragments / vertex","Number of fragments per vertex","Fragment/Vertex"
        2,"Fragments / vertex","Number of fragments per vertex","Fragment/Vertex"
        3,"Fragment / Second","Number of fragments per second","Pixel/s"
        4,"Triangle Acceleration","Number of triangles per ms-ms","Triangle/ms:ms"
        """))

  def test_gpu_render_stages(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages.py'),
        query='''
          SELECT
            g.name AS track_name,
            g.description AS track_desc,
            ts,
            dur,
            s.name AS slice_name,
            depth,
            args.flat_key,
            args.string_value,
            s.context_id,
            render_target,
            render_target_name,
            render_pass,
            render_pass_name,
            command_buffer,
            command_buffer_name,
            submission_id,
            hw_queue_id,
            render_subpasses
          FROM gpu_track g
          JOIN gpu_slice s ON g.id = s.track_id
          LEFT JOIN (
            SELECT arg_set_id, flat_key, string_value
            FROM args
            WHERE args.key IS NULL OR args.key NOT IN (
              'context_id',
              'render_target',
              'render_target_name',
              'render_pass',
              'render_pass_name',
              'command_buffer',
              'command_buffer_name',
              'submission_id',
              'hw_queue_id',
              'render_subpasses',
              'upid'
            )
          ) args USING (arg_set_id)
          ORDER BY ts;
        ''',
        out=Csv('''
          "track_name","track_desc","ts","dur","slice_name","depth","flat_key","string_value","context_id","render_target","render_target_name","render_pass","render_pass_name","command_buffer","command_buffer_name","submission_id","hw_queue_id","render_subpasses"
          "queue 1","queue desc 1",0,5,"render stage(1)",0,"[NULL]","[NULL]",0,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,1,"[NULL]"
          "queue 0","queue desc 0",0,5,"stage 0",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"[NULL]"
          "queue 1","queue desc 1",10,5,"stage 1",0,"description","stage desc 1",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,1,"[NULL]"
          "queue 2","[NULL]",20,5,"stage 2",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,2,"[NULL]"
          "queue 0","queue desc 0",30,5,"stage 3",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"[NULL]"
          "Unknown GPU Queue 3","[NULL]",40,5,"render stage(4)",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,3,"[NULL]"
          "queue 0","queue desc 0",50,5,"stage 0",0,"key1","value1",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"[NULL]"
          "queue 0","queue desc 0",60,5,"stage 0",0,"key1","value1",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"[NULL]"
          "queue 0","queue desc 0",60,5,"stage 0",0,"key2","value2",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"[NULL]"
          "queue 0","queue desc 0",70,5,"stage 0",0,"key1","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"[NULL]"
          "queue 0","queue desc 0",80,5,"stage 2",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"[NULL]"
          "queue 0","queue desc 0",90,5,"stage 0",0,"[NULL]","[NULL]",42,16,"[NULL]",32,"[NULL]",48,"[NULL]",0,0,"[NULL]"
          "queue 0","queue desc 0",100,5,"stage 0",0,"[NULL]","[NULL]",42,16,"[NULL]",16,"[NULL]",16,"command_buffer",0,0,"[NULL]"
          "queue 0","queue desc 0",110,5,"stage 0",0,"[NULL]","[NULL]",42,16,"[NULL]",16,"render_pass",16,"command_buffer",0,0,"[NULL]"
          "queue 0","queue desc 0",120,5,"stage 0",0,"correlation_id","rp:#42",42,16,"framebuffer",16,"render_pass",16,"command_buffer",0,0,"[NULL]"
          "queue 0","queue desc 0",130,5,"stage 0",0,"[NULL]","[NULL]",42,16,"renamed_buffer",0,"[NULL]",0,"[NULL]",0,0,"[NULL]"
          "Unknown GPU Queue 4294967295","[NULL]",140,5,"render stage(18446744073709551615)",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,4294967295,"[NULL]"
          "queue 0","queue desc 0",150,5,"stage 0",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"0"
          "queue 0","queue desc 0",160,5,"stage 0",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"63,64"
          "queue 0","queue desc 0",170,5,"stage 0",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"64"
          "queue 0","queue desc 0",180,5,"stage 0",0,"[NULL]","[NULL]",42,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,0,"3,68,69,70,71"
      '''))

  def test_gpu_render_stages_interned_spec(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages_interned_spec.textproto'),
        query='''
          SELECT
            g.name AS track_name,
            g.description AS track_desc,
            ts,
            dur,
            s.name AS slice_name,
            depth,
            args.flat_key,
            args.string_value,
            s.context_id,
            render_target,
            render_target_name,
            render_pass,
            render_pass_name,
            command_buffer,
            command_buffer_name,
            submission_id,
            hw_queue_id,
            render_subpasses
          FROM gpu_track g
          JOIN gpu_slice s ON g.id = s.track_id
          LEFT JOIN (
            SELECT arg_set_id, flat_key, string_value
            FROM args
            WHERE args.key IS NULL OR args.key NOT IN (
              'context_id',
              'render_target',
              'render_target_name',
              'render_pass',
              'render_pass_name',
              'command_buffer',
              'command_buffer_name',
              'submission_id',
              'hw_queue_id',
              'render_subpasses',
              'upid'
            )
          ) args USING (arg_set_id)
          ORDER BY ts;
        ''',
        out=Csv('''
          "track_name","track_desc","ts","dur","slice_name","depth","flat_key","string_value","context_id","render_target","render_target_name","render_pass","render_pass_name","command_buffer","command_buffer_name","submission_id","hw_queue_id","render_subpasses"
          "vertex","vertex queue",100,10,"binning",0,"description","binning graphics",0,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,1,"[NULL]"
          "fragment","fragment queue",200,10,"render",0,"description","render graphics",0,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,2,"[NULL]"
          "queue2","queue2 description",300,10,"render",0,"description","render graphics",0,0,"[NULL]",0,"[NULL]",0,"[NULL]",0,1,"[NULL]"
        '''))

  def test_vulkan_api_events(self):
    return DiffTestBlueprint(
        trace=Path('vulkan_api_events.py'),
        query="""
        SELECT
          g.name AS track_name,
          g.description AS track_desc,
          ts,
          dur,
          s.name AS slice_name,
          depth,
          s.context_id,
          command_buffer,
          extract_arg(s.arg_set_id, 'command_buffers[0]') as cb0,
          extract_arg(s.arg_set_id, 'command_buffers[1]') as cb1,
          extract_arg(s.arg_set_id, 'command_buffers[2]') as cb2,
          submission_id,
          extract_arg(s.arg_set_id, 'tid') as tid,
          extract_arg(s.arg_set_id, 'pid') as pid
        FROM gpu_track g
        JOIN gpu_slice s ON g.id = s.track_id
        ORDER BY ts;
        """,
        out=Csv('''
          "track_name","track_desc","ts","dur","slice_name","depth","context_id","command_buffer","cb0","cb1","cb2","submission_id","tid","pid"
          "Vulkan Events","[NULL]",10,2,"vkQueueSubmit",0,"[NULL]",100,100,"[NULL]","[NULL]",1,43,42
          "Vulkan Events","[NULL]",20,2,"vkQueueSubmit",0,"[NULL]",200,200,300,400,2,45,44
        '''))

  def test_gpu_log(self):
    return DiffTestBlueprint(
        trace=Path('gpu_log.py'),
        query="""
        SELECT scope, track.name AS track_name, ts, dur, gpu_slice.name AS slice_name,
          key, string_value AS value
        FROM gpu_track
        LEFT JOIN track USING (id)
        LEFT JOIN gpu_slice ON gpu_track.id = gpu_slice.track_id
        LEFT JOIN args USING (arg_set_id)
        ORDER BY ts, slice_name, key;
        """,
        out=Csv("""
        "scope","track_name","ts","dur","slice_name","key","value"
        "gpu_log","GPU Log",1,0,"VERBOSE","message","message0"
        "gpu_log","GPU Log",1,0,"VERBOSE","tag","tag0"
        "gpu_log","GPU Log",2,0,"DEBUG","message","message1"
        "gpu_log","GPU Log",2,0,"DEBUG","tag","tag0"
        "gpu_log","GPU Log",3,0,"INFO","message","message2"
        "gpu_log","GPU Log",3,0,"INFO","tag","tag0"
        "gpu_log","GPU Log",4,0,"ERROR","message","message4"
        "gpu_log","GPU Log",4,0,"ERROR","tag","tag0"
        "gpu_log","GPU Log",4,0,"WARNING","message","message3"
        "gpu_log","GPU Log",4,0,"WARNING","tag","tag0"
        "gpu_log","GPU Log",5,0,"VERBOSE","message","message5"
        "gpu_log","GPU Log",5,0,"VERBOSE","tag","tag1"
        """))

  def test_gpu_render_stages_overlapping(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages_overlapping.py'),
        query='''
          SELECT
            g.name AS track_name,
            ts,
            dur,
            s.name AS slice_name,
            depth
          FROM gpu_track g
          JOIN gpu_slice s ON g.id = s.track_id
          ORDER BY ts;
        ''',
        out=Csv('''
          "track_name","ts","dur","slice_name","depth"
          "queue 1",100,10,"stage 1",0
          "queue 1",105,10,"stage 1",0
        '''))

  def test_gpu_render_stages_per_process(self):
    return DiffTestBlueprint(
        trace=DataPath('gpu_render_stages.pftrace'),
        query='''
          SELECT
            CASE
              WHEN g.name LIKE 'vkcube%' THEN 'vkcube_process'
              WHEN g.name LIKE 'vulkan_sam%' THEN 'vulkan_sam_process'
              ELSE 'other'
            END AS process_indicator,
            COUNT(DISTINCT g.id) AS distinct_track_count,
            COUNT(*) AS total_slices
          FROM gpu_track g
          JOIN gpu_slice s ON g.id = s.track_id
          WHERE g.name LIKE 'vkcube%' OR g.name LIKE 'vulkan_sam%'
          GROUP BY process_indicator
          ORDER BY process_indicator;
        ''',
        out=Csv('''
          "process_indicator","distinct_track_count","total_slices"
          "vkcube_process",6,3220
          "vulkan_sam_process",22,111019
        '''))

  def test_gpu_counter_duplicate_ids_different_sequences(self):
    # With the legacy inline counter_descriptor path, counter specs are
    # globally keyed by counter_id. The second sequence's spec is rejected
    # as a duplicate, so only the first sequence's data is recorded.
    return DiffTestBlueprint(
        trace=TextProto(r"""
          packet {
            trusted_packet_sequence_id: 1
            timestamp: 0
            gpu_counter_event {
              gpu_id: 0
              counter_descriptor {
                specs {
                  counter_id: 1
                  name: "CounterA"
                  description: "desc A"
                }
              }
            }
          }
          packet {
            trusted_packet_sequence_id: 2
            timestamp: 0
            gpu_counter_event {
              gpu_id: 1
              counter_descriptor {
                specs {
                  counter_id: 1
                  name: "CounterA"
                  description: "desc A"
                }
              }
            }
          }
          packet {
            trusted_packet_sequence_id: 1
            timestamp: 10
            gpu_counter_event {
              gpu_id: 0
              counters { counter_id: 1 int_value: 100 }
            }
          }
          packet {
            trusted_packet_sequence_id: 2
            timestamp: 10
            gpu_counter_event {
              gpu_id: 1
              counters { counter_id: 1 int_value: 200 }
            }
          }
          packet {
            trusted_packet_sequence_id: 1
            timestamp: 20
            gpu_counter_event {
              gpu_id: 0
              counters { counter_id: 1 int_value: 150 }
            }
          }
          packet {
            trusted_packet_sequence_id: 2
            timestamp: 20
            gpu_counter_event {
              gpu_id: 1
              counters { counter_id: 1 int_value: 250 }
            }
          }
        """),
        query="""
          SELECT ts, value, name, gpu_id
          FROM counter
          JOIN gpu_counter_track ON counter.track_id = gpu_counter_track.id
          ORDER BY gpu_id, ts;
        """,
        out=Csv("""
          "ts","value","name","gpu_id"
          10,200.000000,"CounterA",0
          10,150.000000,"CounterA",0
          20,250.000000,"CounterA",0
          20,0.000000,"CounterA",0
        """))

  def test_gpu_counter_interned_descriptor(self):
    return DiffTestBlueprint(
        trace=TextProto(r"""
          # Data source 1: register counter descriptor via interning
          packet {
            trusted_packet_sequence_id: 1
            sequence_flags: 1
            interned_data {
              gpu_counter_descriptors {
                iid: 1
                gpu_id: 0
                counter_descriptor {
                  specs {
                    counter_id: 1
                    name: "CounterA"
                    description: "desc A"
                  }
                }
              }
            }
          }
          # Data source 2: same counter_id, different sequence
          packet {
            trusted_packet_sequence_id: 2
            sequence_flags: 1
            interned_data {
              gpu_counter_descriptors {
                iid: 1
                gpu_id: 1
                counter_descriptor {
                  specs {
                    counter_id: 1
                    name: "CounterB"
                    description: "desc B"
                  }
                }
              }
            }
          }
          # Data source 1: reference interned descriptor + emit data
          packet {
            trusted_packet_sequence_id: 1
            sequence_flags: 2
            timestamp: 10
            gpu_counter_event {
              counter_descriptor_iid: 1
              counters { counter_id: 1 int_value: 100 }
            }
          }
          # Data source 2: reference interned descriptor + emit data
          packet {
            trusted_packet_sequence_id: 2
            sequence_flags: 2
            timestamp: 10
            gpu_counter_event {
              counter_descriptor_iid: 1
              counters { counter_id: 1 int_value: 200 }
            }
          }
          # Second batch of data
          packet {
            trusted_packet_sequence_id: 1
            sequence_flags: 2
            timestamp: 20
            gpu_counter_event {
              counter_descriptor_iid: 1
              counters { counter_id: 1 int_value: 150 }
            }
          }
          packet {
            trusted_packet_sequence_id: 2
            sequence_flags: 2
            timestamp: 20
            gpu_counter_event {
              counter_descriptor_iid: 1
              counters { counter_id: 1 int_value: 250 }
            }
          }
        """),
        query="""
          SELECT ts, value, name, gpu_id
          FROM counter
          JOIN gpu_counter_track ON counter.track_id = gpu_counter_track.id
          ORDER BY gpu_id, ts;
        """,
        out=Csv("""
          "ts","value","name","gpu_id"
          10,150.000000,"CounterA",0
          20,0.000000,"CounterA",0
          10,250.000000,"CounterB",1
          20,0.000000,"CounterB",1
        """))
