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

from python.generators.diff_tests.testing import Path, DataPath
from python.generators.diff_tests.testing import Csv, TextProto
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
        SELECT id, name, col_type, nullable, sorted
        FROM perfetto_table_info('__intrinsic_counter');
        """,
        out=Csv("""
        "id","name","col_type","nullable","sorted"
        0,"id","id",0,0
        1,"ts","int64",0,2
        2,"track_id","uint32",0,3
        3,"value","double",0,3
        4,"arg_set_id","uint32",2,3
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

        SELECT id, name, col_type, nullable, sorted from perfetto_table_info('foo');
        """,
        out=Csv("""
        "id","name","col_type","nullable","sorted"
        0,"c","uint32",0,3
        1,"_auto_id","id",0,0
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

  def test_create_perfetto_table_sorted_column(self):
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
        2
        """))

  def test_create_perfetto_table_id_column(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE foo AS
        SELECT 0 AS c
        UNION
        SELECT 1
        UNION
        SELECT 2;

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

  def test_perfetto_table_limit_and_offset(self):
    return DiffTestBlueprint(
        trace=TextProto(''),
        query="""
        CREATE PERFETTO TABLE foo AS
        WITH
          data(x) AS (
            VALUES(1), (2), (3), (4), (5)
          )
        SELECT x FROM data;

        SELECT * FROM foo LIMIT 2 OFFSET 3;
        """,
        out=Csv("""
        "x"
        4
        5
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

          WITH
            ttid AS (
              SELECT thread_track.id
              FROM thread_track
              JOIN thread USING (utid)
              WHERE thread.name = 'android.bg'
              LIMIT 1
            ),
            agg AS MATERIALIZED (
              SELECT
                MIN(track_id) AS min_track_id,
                MAX(name) AS min_name
              FROM __intrinsic_slice
              WHERE track_id = (SELECT id FROM ttid) AND name > "c"
            )
            SELECT
              min_track_id = (SELECT id FROM ttid) AS is_correct_track_id,
              min_name
            FROM agg
        """,
        out=Csv("""
          "is_correct_track_id","min_name"
          1,"virtual bool art::ElfOatFile::Load(const std::string &, bool, bool, bool, art::MemMap *, std::string *)"
        """))

  def test_create_perfetto_index_multiple_smoke(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
          CREATE PERFETTO INDEX idx ON __intrinsic_slice(track_id, name);
          CREATE PERFETTO TABLE bar AS SELECT * FROM slice;

          WITH ttid AS (
            SELECT thread_track.id
            FROM thread_track
            JOIN thread USING (utid)
            WHERE thread.name = 'android.bg'
            LIMIT 1
          )
          SELECT (
            SELECT count()
            FROM bar
            WHERE track_id = (SELECT id FROM ttid) AND dur > 1000 AND name > "b"
          ) AS non_indexes_stats,
          (
            SELECT count()
            FROM __intrinsic_slice
            WHERE track_id = (SELECT id FROM ttid) AND dur > 1000 AND name > "b"
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

          WITH ttid AS (
            SELECT thread_track.id
            FROM thread_track
            JOIN thread USING (utid)
            WHERE thread.name = 'android.bg'
            LIMIT 1
          )
          SELECT MAX(id)
          FROM __intrinsic_slice
          WHERE track_id = (SELECT id FROM ttid);
        """,
        out=Csv("""
        "MAX(id)"
        20745
        """))

  def test_create_and_drop_perfetto_index(self):
    return DiffTestBlueprint(
        trace=DataPath('example_android_trace_30s.pb'),
        query="""
        CREATE PERFETTO INDEX idx ON __intrinsic_slice(track_id, name);
        DROP PERFETTO INDEX idx ON __intrinsic_slice;

        WITH ttid AS (
          SELECT thread_track.id
          FROM thread_track
          JOIN thread USING (utid)
          WHERE thread.name = 'android.bg'
          LIMIT 1
        )
        SELECT MAX(id) FROM __intrinsic_slice
        WHERE track_id = (SELECT id FROM ttid);
        """,
        out=Csv("""
        "MAX(id)"
        20745
        """))

  def test_winscope_proto_to_args_with_defaults_with_simple_fields(self):
    # one set as nondefault, one set as default, and one missing field chosen per field type:
    #   int32: id, z, parent
    #   string: name, pixel_format, type
    #   uint32: flags, layer_stack, owner_uid
    #   bool: invalidate, is_opaque, refresh_pending
    #   float: shadow_radius, corner_radius, requested_corner_radius

    # missing fields for remaining field types:
    #   uint64: curr_frame
    #   enum: hwc_composition_type
    return DiffTestBlueprint(
        trace=Path('../parser/android/surfaceflinger_layers.textproto'),
        query="""
        SELECT flat_key, key, int_value, string_value, real_value FROM __intrinsic_winscope_proto_to_args_with_defaults('surfaceflinger_layer') AS sfl
        WHERE flat_key IN (
            'id',
            "z",
            "parent",
            "name",
            "pixel_format",
            "type",
            "flags",
            "layer_stack",
            "owner_uid",
            "invalidate",
            "is_opaque",
            "refresh_pending",
            "corner_radius",
            "shadow_radius",
            "requested_corner_radius",
            "curr_frame",
            "hwc_composition_type"
          )
        ORDER BY sfl.base64_proto_id, flat_key
        LIMIT 17
        """,
        out=Csv("""
        "flat_key","key","int_value","string_value","real_value"
        "corner_radius","corner_radius","[NULL]","[NULL]",0.000000
        "curr_frame","curr_frame",0,"[NULL]","[NULL]"
        "flags","flags",2,"[NULL]","[NULL]"
        "hwc_composition_type","hwc_composition_type","[NULL]","HWC_TYPE_UNSPECIFIED","[NULL]"
        "id","id",3,"[NULL]","[NULL]"
        "invalidate","invalidate",1,"[NULL]","[NULL]"
        "is_opaque","is_opaque",0,"[NULL]","[NULL]"
        "layer_stack","layer_stack",0,"[NULL]","[NULL]"
        "name","name","[NULL]","Display 0 name=\"Built-in Screen\"#3","[NULL]"
        "owner_uid","owner_uid",0,"[NULL]","[NULL]"
        "parent","parent",0,"[NULL]","[NULL]"
        "pixel_format","pixel_format","[NULL]","","[NULL]"
        "refresh_pending","refresh_pending",0,"[NULL]","[NULL]"
        "requested_corner_radius","requested_corner_radius","[NULL]","[NULL]",0.000000
        "shadow_radius","shadow_radius","[NULL]","[NULL]",0.500000
        "type","type","[NULL]","[NULL]","[NULL]"
        "z","z",0,"[NULL]","[NULL]"
        """))

  def test_winscope_proto_to_args_with_defaults_with_nested_fields(self):
    # barrier_layer: missing
    # blur_regions: set as empty array
    # bounds: all fields set as nondefault in nested proto
    # crop: some fields set as nondefault in nested proto
    # transform: some fields set as default in nested proto
    return DiffTestBlueprint(
        trace=Path('../parser/android/surfaceflinger_layers.textproto'),
        query="""
        SELECT flat_key, key, int_value, string_value, real_value FROM __intrinsic_winscope_proto_to_args_with_defaults('surfaceflinger_layer') AS sfl
        WHERE
          flat_key IN ("barrier_layer", "blur_regions")
          OR flat_key GLOB "bounds.*"
          OR flat_key GLOB "crop.*"
          OR flat_key GLOB "transform.*"
        ORDER BY sfl.base64_proto_id, key
        LIMIT 15
        """,
        out=Csv("""
        "flat_key","key","int_value","string_value","real_value"
        "barrier_layer","barrier_layer","[NULL]","[NULL]","[NULL]"
        "blur_regions","blur_regions","[NULL]","[NULL]","[NULL]"
        "bounds.bottom","bounds.bottom","[NULL]","[NULL]",24000.000000
        "bounds.left","bounds.left","[NULL]","[NULL]",-10800.000000
        "bounds.right","bounds.right","[NULL]","[NULL]",10800.000000
        "bounds.top","bounds.top","[NULL]","[NULL]",-24000.000000
        "crop.bottom","crop.bottom",-1,"[NULL]","[NULL]"
        "crop.left","crop.left",0,"[NULL]","[NULL]"
        "crop.right","crop.right",-1,"[NULL]","[NULL]"
        "crop.top","crop.top",0,"[NULL]","[NULL]"
        "transform.dsdx","transform.dsdx","[NULL]","[NULL]",0.000000
        "transform.dsdy","transform.dsdy","[NULL]","[NULL]",0.000000
        "transform.dtdx","transform.dtdx","[NULL]","[NULL]",0.000000
        "transform.dtdy","transform.dtdy","[NULL]","[NULL]",0.000000
        "transform.type","transform.type",0,"[NULL]","[NULL]"
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
        "displays.id","displays[0].id",1,"[NULL]","[NULL]"
        "displays.is_virtual","displays[0].is_virtual",0,"[NULL]","[NULL]"
        "displays.layer_stack","displays[0].layer_stack",0,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.bottom","displays[0].layer_stack_space_rect.bottom",5,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.left","displays[0].layer_stack_space_rect.left",0,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.right","displays[0].layer_stack_space_rect.right",5,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect.top","displays[0].layer_stack_space_rect.top",0,"[NULL]","[NULL]"
        "displays.name","displays[0].name","[NULL]","[NULL]","[NULL]"
        "displays.size.h","displays[0].size.h",1,"[NULL]","[NULL]"
        "displays.size.w","displays[0].size.w",1,"[NULL]","[NULL]"
        "displays.transform","displays[0].transform","[NULL]","[NULL]","[NULL]"
        "displays.dpi_x","displays[1].dpi_x","[NULL]","[NULL]",0.000000
        "displays.dpi_y","displays[1].dpi_y","[NULL]","[NULL]",0.000000
        "displays.id","displays[1].id",2,"[NULL]","[NULL]"
        "displays.is_virtual","displays[1].is_virtual",0,"[NULL]","[NULL]"
        "displays.layer_stack","displays[1].layer_stack",1,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect","displays[1].layer_stack_space_rect","[NULL]","[NULL]","[NULL]"
        "displays.name","displays[1].name","[NULL]","Display2","[NULL]"
        "displays.size.h","displays[1].size.h",2,"[NULL]","[NULL]"
        "displays.size.w","displays[1].size.w",2,"[NULL]","[NULL]"
        "displays.transform","displays[1].transform","[NULL]","[NULL]","[NULL]"
        "displays.dpi_x","displays[2].dpi_x","[NULL]","[NULL]",0.000000
        "displays.dpi_y","displays[2].dpi_y","[NULL]","[NULL]",0.000000
        "displays.id","displays[2].id",3,"[NULL]","[NULL]"
        "displays.is_virtual","displays[2].is_virtual",0,"[NULL]","[NULL]"
        "displays.layer_stack","displays[2].layer_stack",2,"[NULL]","[NULL]"
        "displays.layer_stack_space_rect","displays[2].layer_stack_space_rect","[NULL]","[NULL]","[NULL]"
        "displays.name","displays[2].name","[NULL]","Display3","[NULL]"
        "displays.size.h","displays[2].size.h",10,"[NULL]","[NULL]"
        "displays.size.w","displays[2].size.w",5,"[NULL]","[NULL]"
        "displays.transform.dsdx","displays[2].transform.dsdx","[NULL]","[NULL]",0.000000
        "displays.transform.dsdy","displays[2].transform.dsdy","[NULL]","[NULL]",0.000000
        "displays.transform.dtdx","displays[2].transform.dtdx","[NULL]","[NULL]",0.000000
        "displays.transform.dtdy","displays[2].transform.dtdy","[NULL]","[NULL]",0.000000
        """))

  def test_winscope_proto_to_args_with_defaults_with_multiple_packets_per_proto(
      self):
    return DiffTestBlueprint(
        trace=Path('../parser/android/shell_transitions.textproto'),
        query="""
          SELECT key, int_value, real_value FROM __intrinsic_winscope_proto_to_args_with_defaults('__intrinsic_window_manager_shell_transition_protos') as tbl
          ORDER BY tbl.base64_proto_id, key
          LIMIT 56
          """,
        out=Csv("""
          "key","int_value","real_value"
          "create_time_ns",76799049027,"[NULL]"
          "finish_time_ns",0,"[NULL]"
          "finish_transaction_id",5604932321954,"[NULL]"
          "flags",0,"[NULL]"
          "merge_request_time_ns",0,"[NULL]"
          "merge_target",0,"[NULL]"
          "merge_time_ns",0,"[NULL]"
          "send_time_ns",76875395422,"[NULL]"
          "shell_abort_time_ns",0,"[NULL]"
          "start_transaction_id",5604932321952,"[NULL]"
          "targets","[NULL]","[NULL]"
          "type",0,"[NULL]"
          "wm_abort_time_ns",0,"[NULL]"
          "create_time_ns",77854865352,"[NULL]"
          "dispatch_time_ns",77899001013,"[NULL]"
          "finish_time_ns",78621610429,"[NULL]"
          "finish_transaction_id",5604932322159,"[NULL]"
          "flags",0,"[NULL]"
          "merge_request_time_ns",0,"[NULL]"
          "merge_target",0,"[NULL]"
          "merge_time_ns",0,"[NULL]"
          "shell_abort_time_ns",0,"[NULL]"
          "start_transaction_id",5604932322158,"[NULL]"
          "starting_window_remove_time_ns",0,"[NULL]"
          "targets","[NULL]","[NULL]"
          "type",0,"[NULL]"
          "wm_abort_time_ns",0,"[NULL]"
          "create_time_ns",82498121051,"[NULL]"
          "finish_time_ns",0,"[NULL]"
          "finish_transaction_id",5604932322347,"[NULL]"
          "flags",0,"[NULL]"
          "merge_request_time_ns",0,"[NULL]"
          "merge_target",0,"[NULL]"
          "merge_time_ns",0,"[NULL]"
          "send_time_ns",82535513345,"[NULL]"
          "shell_abort_time_ns",82536817537,"[NULL]"
          "start_transaction_id",5604932322346,"[NULL]"
          "starting_window_remove_time_ns",0,"[NULL]"
          "targets[0].flags",0,"[NULL]"
          "targets[0].mode",0,"[NULL]"
          "targets[0].window_id",11,"[NULL]"
          "type",0,"[NULL]"
          "wm_abort_time_ns",0,"[NULL]"
          "create_time_ns",76955664017,"[NULL]"
          "finish_time_ns",0,"[NULL]"
          "finish_transaction_id",5604932322029,"[NULL]"
          "flags",0,"[NULL]"
          "merge_request_time_ns",0,"[NULL]"
          "send_time_ns",77277756832,"[NULL]"
          "shell_abort_time_ns",0,"[NULL]"
          "start_transaction_id",5604932322028,"[NULL]"
          "starting_window_remove_time_ns",0,"[NULL]"
          "targets","[NULL]","[NULL]"
          "type",0,"[NULL]"
          "wm_abort_time_ns",0,"[NULL]"
          "starting_window_remove_time_ns",77706603918,"[NULL]"
          """))

  def test_winscope_proto_to_args_with_defaults_with_interned_strings(self):
    return DiffTestBlueprint(
        trace=Path('../parser/android/viewcapture.textproto'),
        query="""
        SELECT flat_key, key, int_value, string_value FROM __intrinsic_winscope_proto_to_args_with_defaults('__intrinsic_viewcapture_view')
        WHERE
          flat_key GLOB '*_iid'
          OR flat_key GLOB '*_name'
          OR flat_key GLOB '*view_id'
        ORDER BY base64_proto_id, key
        LIMIT 8
        """,
        out=Csv("""
        "flat_key","key","int_value","string_value"
        "class_name","class_name","[NULL]","com.android.internal.policy.PhoneWindow@6cec234"
        "view_id","view_id","[NULL]","NO_ID"
        "class_name","class_name","[NULL]","com.android.internal.policy.DecorView"
        "view_id","view_id","[NULL]","STRING DE-INTERNING ERROR"
        "view_id_iid","view_id_iid",3,"[NULL]"
        "class_name","class_name","[NULL]","STRING DE-INTERNING ERROR"
        "class_name_iid","class_name_iid",3,"[NULL]"
        "view_id","view_id","[NULL]","TEST_VIEW_ID"
        """))

  def test_winscope_surfaceflinger_hierarchy_paths(self):
    return DiffTestBlueprint(
        trace=Path('../parser/android/surfaceflinger_layers.textproto'),
        query="""
          SELECT * FROM __intrinsic_winscope_surfaceflinger_hierarchy_path() as tbl
          ORDER BY tbl.id
          LIMIT 10
          """,
        out=Csv("""
          "id","snapshot_id","layer_id","ancestor_id"
          0,0,3,3
          1,0,4,3
          2,0,4,4
          3,1,3,3
          4,1,4,3
          5,1,4,4
          6,2,4294967294,4294967294
          7,2,1,1
          8,2,2,2
          9,2,3,3
          """))
