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


class PerfettoTable(TestSuite):

  def test_create_table(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE foo AS SELECT 42 as a;

        SELECT * FROM foo;
        """,
        out=Csv("""
        "a"
        42
        """))

  def test_replace_table(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        CREATE PERFETTO TABLE foo AS SELECT 42 as a;
        CREATE OR REPLACE PERFETTO TABLE foo AS SELECT 43 as a;

        SELECT * FROM foo;
        """,
        out=Csv("""
        "a"
        43
        """))

  def test_create_perfetto_table_double_metric_run(self):
    return DiffTestBlueprint(
        trace=TextProto(r''),
        query="""
        SELECT RUN_METRIC('android/cpu_info.sql');
        SELECT RUN_METRIC('android/cpu_info.sql');

        SELECT * FROM cluster_core_type;
        """,
        out=Csv("""
        "cluster","core_type"
        0,"little"
        1,"big"
        2,"bigger"
        """))

  def test_perfetto_table_info_static_table(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        SELECT * FROM perfetto_table_info('counter');
        """,
        out=Csv("""
        "id","type","name","col_type","nullable","sorted"
        0,"perfetto_table_info","id","id",0,1
        1,"perfetto_table_info","type","string",0,0
        2,"perfetto_table_info","ts","int64",0,1
        3,"perfetto_table_info","track_id","uint32",0,0
        4,"perfetto_table_info","value","double",0,0
        5,"perfetto_table_info","arg_set_id","uint32",1,0
        """))

  def test_perfetto_table_info_runtime_table(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT * FROM
        (SELECT 2 AS c
        UNION
        SELECT 0 AS c
        UNION
        SELECT 1 AS c)
        ORDER BY c desc;

        SELECT * from perfetto_table_info('foo');
        """,
        out=Csv("""
        "id","type","name","col_type","nullable","sorted"
        0,"perfetto_table_info","c","int64",0,0
        """))

  def test_create_perfetto_table_nullable_column(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT thread_ts FROM slice
        WHERE thread_ts IS NOT NULL;

        SELECT nullable FROM perfetto_table_info('foo')
        WHERE name = 'thread_ts';
        """,
        out=Csv("""
        "nullable"
        0
        """))

  def test_create_perfetto_table_nullable_column(self):
    return DiffTestBlueprint(
        trace=DataPath('android_boot.pftrace'),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT dur FROM slice ORDER BY dur;

        SELECT sorted FROM perfetto_table_info('foo')
        WHERE name = 'dur';
        """,
        out=Csv("""
        "sorted"
        1
        """))

  def test_create_perfetto_table_id_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT 2 AS c
        UNION
        SELECT 4
        UNION
        SELECT 6;

        SELECT col_type FROM perfetto_table_info('foo')
        WHERE name = 'c';
        """,
        out=Csv("""
        "col_type"
        "id"
        """))

  def test_distinct_trivial(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        WITH trivial_count AS (
          SELECT DISTINCT name FROM slice
        ),
        few_results AS (
          SELECT DISTINCT depth FROM slice
        ),
        simple_nullable AS (
          SELECT DISTINCT parent_id FROM slice
        ),
        selector AS (
          SELECT DISTINCT cpu FROM ftrace_event
        )
        SELECT
          (SELECT COUNT(*) FROM trivial_count) AS name,
          (SELECT COUNT(*) FROM few_results) AS depth,
          (SELECT COUNT(*) FROM simple_nullable) AS parent_id,
          (SELECT COUNT(*) FROM selector) AS cpu_from_ftrace;
        """,
        out=Csv("""
        "name","depth","parent_id","cpu_from_ftrace"
        3073,8,4529,8
        """))

  def test_distinct_multi_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE foo AS
        WITH data(a, b) AS (
          VALUES
            -- Needed to defeat any id/sorted detection.
            (2, 3),
            (0, 2),
            (0, 1)
        )
        SELECT * FROM data;

        CREATE TABLE bar AS
        SELECT 1 AS b;

        WITH multi_col_distinct AS (
          SELECT DISTINCT a FROM foo CROSS JOIN bar USING (b)
        ), multi_col_group_by AS (
          SELECT a FROM foo CROSS JOIN bar USING (b) GROUP BY a
        )
        SELECT
          (SELECT COUNT(*) FROM multi_col_distinct) AS cnt_distinct,
          (SELECT COUNT(*) FROM multi_col_group_by) AS cnt_group_by
        """,
        out=Csv("""
        "cnt_distinct","cnt_group_by"
        1,1
        """))

  def test_limit(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        WITH data(a, b) AS (
          VALUES
            (0, 1),
            (1, 10),
            (2, 20),
            (3, 30),
            (4, 40),
            (5, 50)
        )
        SELECT * FROM data LIMIT 3;
        """,
        out=Csv("""
        "a","b"
        0,1
        1,10
        2,20
        """))

  def test_limit_and_offset_in_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        WITH data(a, b) AS (
          VALUES
            (0, 1),
            (1, 10),
            (2, 20),
            (3, 30),
            (4, 40),
            (5, 50),
            (6, 60),
            (7, 70),
            (8, 80),
            (9, 90)
        )
        SELECT * FROM data LIMIT 2 OFFSET 3;
        """,
        out=Csv("""
        "a","b"
        3,30
        4,40
        """))

  def test_limit_and_offset_not_in_bounds(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        WITH data(a, b) AS (
          VALUES
            (0, 1),
            (1, 10),
            (2, 20),
            (3, 30),
            (4, 40),
            (5, 50),
            (6, 60),
            (7, 70),
            (8, 80),
            (9, 90)
        )
        SELECT * FROM data LIMIT 5 OFFSET 6;
        """,
        out=Csv("""
        "a","b"
        6,60
        7,70
        8,80
        9,90
        """))

  def test_max(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO MACRO max(col ColumnName)
        RETURNS TableOrSubquery AS (
          SELECT id, $col
          FROM slice
          ORDER BY $col DESC
          LIMIT 1
        );

        SELECT
          (SELECT id FROM max!(id)) AS id,
          (SELECT id FROM max!(dur)) AS numeric,
          (SELECT id FROM max!(name)) AS string,
          (SELECT id FROM max!(parent_id)) AS nullable;
        """,
        out=Csv("""
        "id","numeric","string","nullable"
        20745,2698,148,20729
        """))

  def test_min(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO MACRO min(col ColumnName)
        RETURNS TableOrSubquery AS (
          SELECT id, $col
          FROM slice
          ORDER BY $col ASC
          LIMIT 1
        );

        SELECT
          (SELECT id FROM min!(id)) AS id,
          (SELECT id FROM min!(dur)) AS numeric,
          (SELECT id FROM min!(name)) AS string,
          (SELECT id FROM min!(parent_id)) AS nullable;
        """,
        out=Csv("""
        "id","numeric","string","nullable"
        0,3111,460,0
        """))

  def test_create_perfetto_index(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX foo ON __intrinsic_slice(track_id);
        CREATE PERFETTO INDEX foo_name ON __intrinsic_slice(name);

        SELECT
          COUNT() FILTER (WHERE track_id > 10) AS track_idx,
          COUNT() FILTER (WHERE name > "g") AS name_idx
        FROM __intrinsic_slice;
        """,
        out=Csv("""
        "track_idx","name_idx"
        20717,7098
        """))

  def test_create_perfetto_index_multiple_cols(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX foo ON __intrinsic_slice(track_id, name);

        SELECT
          MIN(track_id) AS min_track_id,
          MAX(name) AS min_name
        FROM __intrinsic_slice
        WHERE track_id = 13 AND name > "c"
        """,
        out=Csv("""
        "min_track_id","min_name"
        13,"virtual bool art::ElfOatFile::Load(const std::string &, bool, bool, bool, art::MemMap *, std::string *)"
        """))

  def test_create_perfetto_index_multiple_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX idx ON __intrinsic_slice(track_id, name);
        CREATE PERFETTO TABLE bar AS SELECT * FROM slice;

       SELECT (
          SELECT count()
          FROM bar
          WHERE track_id = 13 AND dur > 1000 AND name > "b"
        ) AS non_indexes_stats,
        (
          SELECT count()
          FROM slice
          WHERE track_id = 13 AND dur > 1000 AND name > "b"
        ) AS indexed_stats
        """,
        out=Csv("""
        "non_indexes_stats","indexed_stats"
        39,39
        """))

  def test_create_or_replace_perfetto_index(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX idx ON __intrinsic_slice(track_id, name);
        CREATE OR REPLACE PERFETTO INDEX idx ON __intrinsic_slice(name);

       SELECT MAX(id) FROM slice WHERE track_id = 13;
        """,
        out=Csv("""
        "MAX(id)"
        20745
        """))

  def test_create_or_replace_perfetto_index(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX idx ON __intrinsic_slice(track_id, name);
        DROP PERFETTO INDEX idx ON __intrinsic_slice;

        SELECT MAX(id) FROM slice WHERE track_id = 13;
        """,
        out=Csv("""
        "MAX(id)"
        20745
        """))

  def test_winscope_proto_to_args_with_defaults_with_nested_fields(self):
    return DiffTestBlueprint(
        trace=Path('../parser/android/surfaceflinger_layers.textproto'),
        query="""
        SELECT flat_key, key, int_value, string_value, real_value FROM __intrinsic_winscope_proto_to_args_with_defaults('surfaceflinger_layer') AS sfl
        ORDER BY sfl.base64_proto_id, key
        LIMIT 95
        """,
        out=Csv("""
        "flat_key","key","int_value","string_value","real_value"
        "active_buffer","active_buffer","[NULL]","[NULL]","[NULL]"
        "app_id","app_id",0,"[NULL]","[NULL]"
        "background_blur_radius","background_blur_radius",0,"[NULL]","[NULL]"
        "barrier_layer","barrier_layer","[NULL]","[NULL]","[NULL]"
        "blur_regions","blur_regions","[NULL]","[NULL]","[NULL]"
        "bounds.bottom","bounds.bottom","[NULL]","[NULL]",24000.000000
        "bounds.left","bounds.left","[NULL]","[NULL]",-10800.000000
        "bounds.right","bounds.right","[NULL]","[NULL]",10800.000000
        "bounds.top","bounds.top","[NULL]","[NULL]",-24000.000000
        "buffer_transform","buffer_transform","[NULL]","[NULL]","[NULL]"
        "children","children[0]",4,"[NULL]","[NULL]"
        "children","children[1]",35,"[NULL]","[NULL]"
        "children","children[2]",43,"[NULL]","[NULL]"
        "children","children[3]",45,"[NULL]","[NULL]"
        "children","children[4]",44,"[NULL]","[NULL]"
        "children","children[5]",77,"[NULL]","[NULL]"
        "children","children[6]",87,"[NULL]","[NULL]"
        "color.a","color.a","[NULL]","[NULL]",1.000000
        "color.b","color.b","[NULL]","[NULL]",-1.000000
        "color.g","color.g","[NULL]","[NULL]",-1.000000
        "color.r","color.r","[NULL]","[NULL]",-1.000000
        "color_transform","color_transform","[NULL]","[NULL]","[NULL]"
        "corner_radius","corner_radius","[NULL]","[NULL]",0.000000
        "corner_radius_crop","corner_radius_crop","[NULL]","[NULL]","[NULL]"
        "crop.bottom","crop.bottom",-1,"[NULL]","[NULL]"
        "crop.left","crop.left",0,"[NULL]","[NULL]"
        "crop.right","crop.right",-1,"[NULL]","[NULL]"
        "crop.top","crop.top",0,"[NULL]","[NULL]"
        "curr_frame","curr_frame",0,"[NULL]","[NULL]"
        "damage_region","damage_region","[NULL]","[NULL]","[NULL]"
        "dataspace","dataspace","[NULL]","BT709 sRGB Full range","[NULL]"
        "destination_frame.bottom","destination_frame.bottom",-1,"[NULL]","[NULL]"
        "destination_frame.left","destination_frame.left",0,"[NULL]","[NULL]"
        "destination_frame.right","destination_frame.right",-1,"[NULL]","[NULL]"
        "destination_frame.top","destination_frame.top",0,"[NULL]","[NULL]"
        "effective_scaling_mode","effective_scaling_mode",0,"[NULL]","[NULL]"
        "effective_transform","effective_transform","[NULL]","[NULL]","[NULL]"
        "final_crop","final_crop","[NULL]","[NULL]","[NULL]"
        "flags","flags",2,"[NULL]","[NULL]"
        "hwc_composition_type","hwc_composition_type","[NULL]","HWC_TYPE_UNSPECIFIED","[NULL]"
        "hwc_crop","hwc_crop","[NULL]","[NULL]","[NULL]"
        "hwc_frame","hwc_frame","[NULL]","[NULL]","[NULL]"
        "hwc_transform","hwc_transform",0,"[NULL]","[NULL]"
        "id","id",3,"[NULL]","[NULL]"
        "input_window_info","input_window_info","[NULL]","[NULL]","[NULL]"
        "invalidate","invalidate",1,"[NULL]","[NULL]"
        "is_opaque","is_opaque",0,"[NULL]","[NULL]"
        "is_protected","is_protected",0,"[NULL]","[NULL]"
        "is_relative_of","is_relative_of",0,"[NULL]","[NULL]"
        "is_trusted_overlay","is_trusted_overlay",0,"[NULL]","[NULL]"
        "layer_stack","layer_stack",0,"[NULL]","[NULL]"
        "metadata","metadata","[NULL]","[NULL]","[NULL]"
        "name","name","[NULL]","Display 0 name=\"Built-in Screen\"#3","[NULL]"
        "original_id","original_id",0,"[NULL]","[NULL]"
        "owner_uid","owner_uid",1000,"[NULL]","[NULL]"
        "parent","parent",0,"[NULL]","[NULL]"
        "pixel_format","pixel_format","[NULL]","Unknown/None","[NULL]"
        "position","position","[NULL]","[NULL]","[NULL]"
        "queued_frames","queued_frames",0,"[NULL]","[NULL]"
        "refresh_pending","refresh_pending",0,"[NULL]","[NULL]"
        "relatives","relatives","[NULL]","[NULL]","[NULL]"
        "requested_color.a","requested_color.a","[NULL]","[NULL]",1.000000
        "requested_color.b","requested_color.b","[NULL]","[NULL]",-1.000000
        "requested_color.g","requested_color.g","[NULL]","[NULL]",-1.000000
        "requested_color.r","requested_color.r","[NULL]","[NULL]",-1.000000
        "requested_corner_radius","requested_corner_radius","[NULL]","[NULL]",0.000000
        "requested_position","requested_position","[NULL]","[NULL]","[NULL]"
        "requested_transform.dsdx","requested_transform.dsdx","[NULL]","[NULL]",0.000000
        "requested_transform.dsdy","requested_transform.dsdy","[NULL]","[NULL]",0.000000
        "requested_transform.dtdx","requested_transform.dtdx","[NULL]","[NULL]",0.000000
        "requested_transform.dtdy","requested_transform.dtdy","[NULL]","[NULL]",0.000000
        "requested_transform.type","requested_transform.type",0,"[NULL]","[NULL]"
        "screen_bounds.bottom","screen_bounds.bottom","[NULL]","[NULL]",24000.000000
        "screen_bounds.left","screen_bounds.left","[NULL]","[NULL]",-10800.000000
        "screen_bounds.right","screen_bounds.right","[NULL]","[NULL]",10800.000000
        "screen_bounds.top","screen_bounds.top","[NULL]","[NULL]",-24000.000000
        "shadow_radius","shadow_radius","[NULL]","[NULL]",0.000000
        "size","size","[NULL]","[NULL]","[NULL]"
        "source_bounds.bottom","source_bounds.bottom","[NULL]","[NULL]",24000.000000
        "source_bounds.left","source_bounds.left","[NULL]","[NULL]",-10800.000000
        "source_bounds.right","source_bounds.right","[NULL]","[NULL]",10800.000000
        "source_bounds.top","source_bounds.top","[NULL]","[NULL]",-24000.000000
        "transform.dsdx","transform.dsdx","[NULL]","[NULL]",0.000000
        "transform.dsdy","transform.dsdy","[NULL]","[NULL]",0.000000
        "transform.dtdx","transform.dtdx","[NULL]","[NULL]",0.000000
        "transform.dtdy","transform.dtdy","[NULL]","[NULL]",0.000000
        "transform.type","transform.type",0,"[NULL]","[NULL]"
        "transparent_region","transparent_region","[NULL]","[NULL]","[NULL]"
        "trusted_overlay","trusted_overlay","[NULL]","UNSET","[NULL]"
        "type","type","[NULL]","[NULL]","[NULL]"
        "visible_region","visible_region","[NULL]","[NULL]","[NULL]"
        "window_type","window_type",0,"[NULL]","[NULL]"
        "z","z",0,"[NULL]","[NULL]"
        "z_order_relative_of","z_order_relative_of",0,"[NULL]","[NULL]"
        "active_buffer","active_buffer","[NULL]","[NULL]","[NULL]"
        """))

  def test_winscope_proto_to_args_with_defaults_with_repeated_fields(self):
    return DiffTestBlueprint(
        trace=Path('../parser/android/surfaceflinger_layers.textproto'),
        query="""
        SELECT flat_key, key, int_value, string_value, real_value FROM __intrinsic_winscope_proto_to_args_with_defaults('surfaceflinger_layers_snapshot') AS sfs
        WHERE key != "hwc_blob"
        ORDER BY sfs.base64_proto_id DESC, key ASC
        LIMIT 36
        """,
        out=Csv("""
        "flat_key","key","int_value","string_value","real_value"
        "displays.dpi_x","displays[0].dpi_x","[NULL]","[NULL]",0.000000
        "displays.dpi_y","displays[0].dpi_y","[NULL]","[NULL]",0.000000
        "displays.id","displays[0].id",4619827677550801152,"[NULL]","[NULL]"
        "displays.is_virtual","displays[0].is_virtual",0,"[NULL]","[NULL]"
        "displays.layer_stack","displays[0].layer_stack",0,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.bottom","displays[0].layer_stack_space_rect.bottom",2400,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.left","displays[0].layer_stack_space_rect.left",0,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.right","displays[0].layer_stack_space_rect.right",1080,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.top","displays[0].layer_stack_space_rect.top",0,"[NULL]","[NULL]"
        "displays.name","displays[0].name","[NULL]","Common Panel","[NULL]"
        "displays.size.h","displays[0].size.h",2400,"[NULL]","[NULL]"
        "displays.size.w","displays[0].size.w",1080,"[NULL]","[NULL]"
        "displays.transform.dsdx","displays[0].transform.dsdx","[NULL]","[NULL]",0.000000
        "displays.transform.dsdy","displays[0].transform.dsdy","[NULL]","[NULL]",0.000000
        "displays.transform.dtdx","displays[0].transform.dtdx","[NULL]","[NULL]",0.000000
        "displays.transform.dtdy","displays[0].transform.dtdy","[NULL]","[NULL]",0.000000
        "displays.transform.type","displays[0].transform.type",0,"[NULL]","[NULL]"
        "displays.dpi_x","displays[1].dpi_x","[NULL]","[NULL]",0.000000
        "displays.dpi_y","displays[1].dpi_y","[NULL]","[NULL]",0.000000
        "displays.id","displays[1].id",4619827677550801153,"[NULL]","[NULL]"
        "displays.is_virtual","displays[1].is_virtual",0,"[NULL]","[NULL]"
        "displays.layer_stack","displays[1].layer_stack",0,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.bottom","displays[1].layer_stack_space_rect.bottom",2400,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.left","displays[1].layer_stack_space_rect.left",0,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.right","displays[1].layer_stack_space_rect.right",1080,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.top","displays[1].layer_stack_space_rect.top",0,"[NULL]","[NULL]"
        "displays.name","displays[1].name","[NULL]","Common Panel","[NULL]"
        "displays.size.h","displays[1].size.h",2400,"[NULL]","[NULL]"
        "displays.size.w","displays[1].size.w",1080,"[NULL]","[NULL]"
        "displays.transform","displays[1].transform","[NULL]","[NULL]","[NULL]"
        "elapsed_realtime_nanos","elapsed_realtime_nanos",2749500341063,"[NULL]","[NULL]"
        "excludes_composition_state","excludes_composition_state",0,"[NULL]","[NULL]"
        "missed_entries","missed_entries",0,"[NULL]","[NULL]"
        "vsync_id","vsync_id",24767,"[NULL]","[NULL]"
        "where","where","[NULL]","bufferLatched","[NULL]"
        "displays.dpi_x","displays[0].dpi_x","[NULL]","[NULL]",0.000000
        """))
