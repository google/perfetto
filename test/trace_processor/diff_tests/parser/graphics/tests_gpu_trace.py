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
        11,5.000000,"Vertex / Second",0,"Number of vertices per second","25/22"
        12,7.000000,"Fragment / Second",0,"Number of fragments per second","26/22"
        14,0.000000,"Triangle Acceleration",0,"Number of triangles per ms-ms","27/21:21"
        21,10.000000,"Vertex / Second",0,"Number of vertices per second","25/22"
        22,14.000000,"Fragment / Second",0,"Number of fragments per second","26/22"
        24,9.000000,"Triangle Acceleration",0,"Number of triangles per ms-ms","27/21:21"
        31,15.000000,"Vertex / Second",0,"Number of vertices per second","25/22"
        32,21.000000,"Fragment / Second",0,"Number of fragments per second","26/22"
        34,7.000000,"Triangle Acceleration",0,"Number of triangles per ms-ms","27/21:21"
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
        0,"GPU Frequency","clock speed","/22"
        3,"Fragments / vertex","Number of fragments per vertex","39/25"
        2,"Fragments / vertex","Number of fragments per vertex","39/25"
        3,"Fragment / Second","Number of fragments per second","26/22"
        4,"Triangle Acceleration","Number of triangles per ms-ms","27/21:21"
        """))

  def test_gpu_render_stages(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages.py'),
        query=Path('gpu_render_stages_test.sql'),
        out=Path('gpu_render_stages.out'))

  def test_gpu_render_stages_interned_spec(self):
    return DiffTestBlueprint(
        trace=Path('gpu_render_stages_interned_spec.textproto'),
        query=Path('gpu_render_stages_test.sql'),
        out=Path('gpu_render_stages_interned_spec.out'))

  def test_vulkan_api_events(self):
    return DiffTestBlueprint(
        trace=Path('vulkan_api_events.py'),
        query="""
        SELECT track.name AS track_name, gpu_track.description AS track_desc, ts, dur,
          gpu_slice.name AS slice_name, depth, flat_key, int_value,
          gpu_slice.context_id, command_buffer, submission_id
        FROM gpu_track
        LEFT JOIN track USING (id)
        JOIN gpu_slice ON gpu_track.id = gpu_slice.track_id
        LEFT JOIN args ON gpu_slice.arg_set_id = args.arg_set_id
        ORDER BY ts;
        """,
        out=Path('vulkan_api_events.out'))

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
