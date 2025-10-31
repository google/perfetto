// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {createGraphFromSql} from './sql_json_handler';
import {SerializedGraph, SerializedNode} from './json_handler';
import {SqlSourceSerializedState} from './query_builder/nodes/sources/sql_source';
import {NodeType} from './query_node';

describe('createGraphFromSql', () => {
  it('should create a graph from a simple SQL WITH statement', () => {
    const sql = `
      WITH a AS (SELECT 1),
           b AS (SELECT * FROM a),
           c AS (SELECT * FROM b)
      SELECT * FROM c
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(4);
    expect(graph.rootNodeIds).toEqual(['a']);

    const nodeA = graph.nodes.find((n: SerializedNode) => n.nodeId === 'a');
    const nodeB = graph.nodes.find((n: SerializedNode) => n.nodeId === 'b');
    const nodeC = graph.nodes.find((n: SerializedNode) => n.nodeId === 'c');
    const nodeOutput = graph.nodes.find(
      (n: SerializedNode) => n.nodeId === 'output',
    );

    expect(nodeA).toBeDefined();
    expect(nodeB).toBeDefined();
    expect(nodeC).toBeDefined();
    expect(nodeOutput).toBeDefined();

    expect(nodeA!.type).toBe(NodeType.kSqlSource);
    expect((nodeA!.state as SqlSourceSerializedState).sql).toBe('SELECT 1');
    expect(nodeA!.nextNodes).toEqual(['b']);
    expect(nodeA!.prevNodes).toEqual([]);

    expect(nodeB!.type).toBe(NodeType.kSqlSource);
    expect((nodeB!.state as SqlSourceSerializedState).sql).toBe(
      'SELECT * FROM $a',
    );
    expect(nodeB!.nextNodes).toEqual(['c']);
    expect(nodeB!.prevNodes).toEqual(['a']);

    expect(nodeC!.type).toBe(NodeType.kSqlSource);
    expect((nodeC!.state as SqlSourceSerializedState).sql).toBe(
      'SELECT * FROM $b',
    );
    expect(nodeC!.nextNodes).toEqual(['output']);
    expect(nodeC!.prevNodes).toEqual(['b']);

    expect(nodeOutput!.type).toBe(NodeType.kSqlSource);
    expect((nodeOutput!.state as SqlSourceSerializedState).sql).toBe(
      'SELECT * FROM $c',
    );
    expect(nodeOutput!.nextNodes).toEqual([]);
    expect(nodeOutput!.prevNodes).toEqual(['c']);
  });

  it('should throw an error for malformed SQL without SELECT', () => {
    const sql = 'WITH a AS (SELECT 1)';
    expect(() => createGraphFromSql(sql)).toThrow(
      'Malformed SQL: No SELECT statement found after WITH clause.',
    );
  });

  it('should throw an error for malformed CTE clause', () => {
    const sql = 'WITH a (SELECT 1) SELECT * FROM a';
    expect(() => createGraphFromSql(sql)).toThrow(
      'Malformed CTE clause: a (SELECT 1)',
    );
  });

  it('should handle lowercase "as"', () => {
    const sql = 'with x as (select * from slice) select * from x';
    expect(() => createGraphFromSql(sql)).not.toThrow();
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);
    expect(graph.nodes.length).toBe(2);
  });

  it('should handle comments', () => {
    const sql = `
      WITH a AS (
        -- This is a comment
        SELECT 1
      ),
      /* This is another comment */
      b AS (SELECT * FROM a)
      SELECT * FROM b
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);
    expect(graph.nodes.length).toBe(3);
  });

  it('should handle string literals with commas', () => {
    const sql = `
      WITH a AS (SELECT 'hello, world'),
           b AS (SELECT * FROM a)
      SELECT * FROM b
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);
    expect(graph.nodes.length).toBe(3);
  });

  it('should handle PERFETTO INCLUDE MODULE statements', () => {
    const sql = `
      PERFETTO INCLUDE MODULE android.slices;
      PERFETTO INCLUDE MODULE experimental.slices;

      WITH a AS (SELECT 1)
      SELECT * FROM a
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(2);
    expect(graph.rootNodeIds).toEqual(['a']);

    const nodeA = graph.nodes.find((n: SerializedNode) => n.nodeId === 'a');
    expect(nodeA).toBeDefined();
    expect((nodeA!.state as SqlSourceSerializedState).sql).toBe(
      'PERFETTO INCLUDE MODULE android.slices;\nPERFETTO INCLUDE MODULE experimental.slices;\nSELECT 1',
    );
  });

  it('should handle real-world query with GLOB and STR_SPLIT', () => {
    const sql = `
      INCLUDE PERFETTO MODULE android.frames.jank_type;
      INCLUDE PERFETTO MODULE android.frames.timeline;

      WITH actual_timeline_with_vsync AS (
        SELECT
          *,
          CAST(name AS INTEGER) AS vsync
        FROM actual_frame_timeline_slice
        WHERE dur > 0
      ),
      timeline_filtered AS (
        SELECT
          boundary.cuj_id,
          timeline.*
        FROM android_jank_cuj_vsync_boundary boundary
        JOIN actual_timeline_with_vsync timeline
          ON timeline.vsync >= boundary.vsync_min AND timeline.vsync <= boundary.vsync_max
        WHERE
          boundary.layer_id IS NULL
          OR (
            timeline.layer_name GLOB '*#*'
            AND boundary.layer_id = CAST(STR_SPLIT(timeline.layer_name, '#', 1) AS INTEGER)
          )
      )
      SELECT
        cuj_id,
        vsync,
        MAX(android_is_app_jank_type(jank_type)) AS app_missed,
        MAX(android_is_sf_jank_type(jank_type)) AS sf_missed,
        IFNULL(MAX(sf_callback_missed), 0) AS sf_callback_missed,
        IFNULL(MAX(hwui_callback_missed), 0) AS hwui_callback_missed,
        MIN(on_time_finish) AS on_time_finish,
        MAX(timeline.ts + timeline.dur) AS ts_end_actual,
        MAX(timeline.dur) AS dur,
        COALESCE(MAX(expected.dur), 16600000) AS dur_expected,
        COUNT(DISTINCT timeline.layer_name) as number_of_layers_for_frame,
        MAX(timeline.layer_name) as frame_layer_name
      FROM timeline_filtered timeline
      LEFT JOIN expected_frame_timeline_slice expected
        ON expected.upid = timeline.upid AND expected.name = timeline.name
      LEFT JOIN _vsync_missed_callback missed_callback USING(vsync)
      GROUP BY cuj_id, vsync;
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(3);
    expect(graph.rootNodeIds).toEqual(['actual_timeline_with_vsync']);

    const nodeA = graph.nodes.find(
      (n) => n.nodeId === 'actual_timeline_with_vsync',
    );
    const nodeB = graph.nodes.find((n) => n.nodeId === 'timeline_filtered');
    const nodeOutput = graph.nodes.find((n) => n.nodeId === 'output');

    expect(nodeA).toBeDefined();
    expect(nodeB).toBeDefined();
    expect(nodeOutput).toBeDefined();

    expect(nodeA!.nextNodes).toEqual(['timeline_filtered']);
    expect(nodeA!.prevNodes).toEqual([]);

    expect(nodeB!.nextNodes).toEqual(['output']);
    expect(nodeB!.prevNodes).toEqual(['actual_timeline_with_vsync']);

    expect(nodeOutput!.prevNodes).toEqual(['timeline_filtered']);
  });

  it('should handle real-world query with ROW_NUMBER and OVER', () => {
    const sql = `
      WITH frame_base AS (
        SELECT
          cuj_id,
          vsync,
          boundary.ts,
          boundary.ts_expected,
          boundary.ts_do_frame_start
        FROM _android_jank_cuj_do_frames do_frame
        JOIN android_jank_cuj_main_thread_frame_boundary boundary USING (cuj_id, vsync)
      ),
      frame_with_gpu AS (
        SELECT
          fb.cuj_id,
          fb.vsync,
          fb.ts,
          fb.ts_expected,
          fb.ts_do_frame_start,
          COUNT(fence.fence_idx) AS gpu_fence_count,
          COUNT(fence.fence_idx) > 0 AS drew_anything
        FROM frame_base fb
        JOIN android_jank_cuj_draw_frame_slice draw_frame USING (cuj_id, vsync)
        LEFT JOIN android_jank_cuj_gpu_completion_fence fence ON
          draw_frame.cuj_id = fence.cuj_id AND
          draw_frame.vsync = fence.vsync AND
          draw_frame.id = fence.draw_frame_slice_id
        GROUP BY fb.cuj_id, fb.vsync, fb.ts, fb.ts_do_frame_start
      ),
      frame_with_numbers AS (
        SELECT
          *,
          ROW_NUMBER() OVER (PARTITION BY cuj_id ORDER BY vsync ASC) AS frame_number
        FROM frame_with_gpu
      )
      SELECT
        fwn.frame_number,
        fwn.cuj_id,
        fwn.vsync,
        fwn.ts,
        fwn.ts_expected,
        fwn.ts_do_frame_start,
        fwn.gpu_fence_count,
        fwn.drew_anything,
        timeline.app_missed,
        timeline.sf_missed,
        timeline.sf_callback_missed,
        timeline.hwui_callback_missed,
        timeline.on_time_finish,
        timeline.ts_end_actual - fwn.ts AS dur,
        timeline.ts_end_actual - fwn.ts_do_frame_start AS dur_unadjusted,
        timeline.dur_expected,
        timeline.ts_end_actual AS ts_end
      FROM frame_with_numbers fwn
      JOIN android_jank_cuj_frame_timeline timeline USING (cuj_id, vsync);
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(4);
    expect(graph.rootNodeIds).toEqual(['frame_base']);

    const nodeA = graph.nodes.find((n) => n.nodeId === 'frame_base');
    const nodeB = graph.nodes.find((n) => n.nodeId === 'frame_with_gpu');
    const nodeC = graph.nodes.find((n) => n.nodeId === 'frame_with_numbers');
    const nodeOutput = graph.nodes.find((n) => n.nodeId === 'output');

    expect(nodeA).toBeDefined();
    expect(nodeB).toBeDefined();
    expect(nodeC).toBeDefined();
    expect(nodeOutput).toBeDefined();

    expect(nodeA!.nextNodes).toEqual(['frame_with_gpu']);
    expect(nodeA!.prevNodes).toEqual([]);

    expect(nodeB!.nextNodes).toEqual(['frame_with_numbers']);
    expect(nodeB!.prevNodes).toEqual(['frame_base']);

    expect(nodeC!.nextNodes).toEqual(['output']);
    expect(nodeC!.prevNodes).toEqual(['frame_with_gpu']);

    expect(nodeOutput!.prevNodes).toEqual(['frame_with_numbers']);
  });

  it('should handle real-world query with multiple joins and COALESCE', () => {
    const sql = `
      WITH android_jank_cuj_timeline_sf_frame AS (
          SELECT DISTINCT
            cuj_id,
            CAST(timeline.name AS INTEGER) AS vsync,
            timeline.display_frame_token
          FROM android_jank_cuj_vsync_boundary boundary
          JOIN actual_frame_timeline_slice timeline
            ON
              boundary.upid = timeline.upid
              AND CAST(timeline.name AS INTEGER) >= vsync_min
              AND CAST(timeline.name AS INTEGER) <= vsync_max
          WHERE
              boundary.layer_id IS NULL
            OR (
              timeline.layer_name GLOB '*#*'
              AND boundary.layer_id = CAST(STR_SPLIT(timeline.layer_name, '#', 1) AS INTEGER))
      ),
      sf_frames_with_jank AS (
          SELECT
            boundary.cuj_id,
            boundary.vsync,
            boundary.ts,
            boundary.ts_main_thread_start,
            boundary.ts_end,
            boundary.dur,
            actual_timeline.jank_tag = 'Self Jank' AS sf_missed,
            jank_tag,
            jank_type,
            prediction_type,
            present_type,
            gpu_composition,
            expected_timeline.dur as expected_dur,
            actual_timeline.upid,
            actual_timeline.name
          FROM android_jank_cuj_sf_main_thread_frame_boundary boundary
          JOIN android_jank_cuj_sf_process sf_process
          JOIN actual_frame_timeline_slice actual_timeline
            ON actual_timeline.upid = sf_process.upid
              AND boundary.vsync = CAST(actual_timeline.name AS INTEGER)
          JOIN android_jank_cuj_timeline_sf_frame ft
            ON CAST(actual_timeline.name AS INTEGER) = ft.display_frame_token
              AND boundary.cuj_id = ft.cuj_id
          LEFT JOIN expected_frame_timeline_slice expected_timeline
            ON expected_timeline.upid = actual_timeline.upid
              AND expected_timeline.name = actual_timeline.name
      ),
      android_jank_cuj_sf_frame_base AS (
          SELECT DISTINCT
            cuj_id,
            vsync,
            ts,
            ts_main_thread_start,
            ts_end,
            dur,
            sf_missed,
            NULL AS app_missed, -- for simplicity align schema with android_jank_cuj_frame
            jank_tag,
            jank_type,
            prediction_type,
            present_type,
            gpu_composition,
            -- In case expected timeline is missing, as a fallback we use the typical frame deadline
            -- for 60Hz.
            -- See similar expression in android_jank_cuj_frame_timeline.
            COALESCE(expected_dur, 16600000) AS dur_expected
          FROM sf_frames_with_jank
      )
      SELECT
       *,
       ROW_NUMBER() OVER (PARTITION BY cuj_id ORDER BY vsync ASC) AS frame_number
      FROM android_jank_cuj_sf_frame_base;
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(4);
    expect(graph.rootNodeIds).toEqual(['android_jank_cuj_timeline_sf_frame']);

    const nodeA = graph.nodes.find(
      (n) => n.nodeId === 'android_jank_cuj_timeline_sf_frame',
    );
    const nodeB = graph.nodes.find((n) => n.nodeId === 'sf_frames_with_jank');
    const nodeC = graph.nodes.find(
      (n) => n.nodeId === 'android_jank_cuj_sf_frame_base',
    );
    const nodeOutput = graph.nodes.find((n) => n.nodeId === 'output');

    expect(nodeA).toBeDefined();
    expect(nodeB).toBeDefined();
    expect(nodeC).toBeDefined();
    expect(nodeOutput).toBeDefined();

    expect(nodeA!.nextNodes).toEqual(['sf_frames_with_jank']);
    expect(nodeA!.prevNodes).toEqual([]);

    expect(nodeB!.nextNodes).toEqual(['android_jank_cuj_sf_frame_base']);
    expect(nodeB!.prevNodes).toEqual(['android_jank_cuj_timeline_sf_frame']);

    expect(nodeC!.nextNodes).toEqual(['output']);
    expect(nodeC!.prevNodes).toEqual(['sf_frames_with_jank']);

    expect(nodeOutput!.prevNodes).toEqual(['android_jank_cuj_sf_frame_base']);
  });

  it('should handle comments with commas', () => {
    const sql = `
      WITH a AS (
        /* This is a comment, with a comma */
        SELECT 1
      ),
      b AS (SELECT * FROM a)
      SELECT * FROM b
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);
    expect(graph.nodes.length).toBe(3);
  });

  it('should handle a node referencing two nodes (JOIN)', () => {
    const sql = `
      WITH a AS (SELECT 1 as id),
           b AS (SELECT 2 as id),
           c AS (SELECT * FROM a JOIN b ON a.id = b.id)
      SELECT * FROM c
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(4);
    expect(graph.rootNodeIds).toEqual(['a', 'b']);

    const nodeA = graph.nodes.find((n) => n.nodeId === 'a');
    const nodeB = graph.nodes.find((n) => n.nodeId === 'b');
    const nodeC = graph.nodes.find((n) => n.nodeId === 'c');
    const nodeOutput = graph.nodes.find((n) => n.nodeId === 'output');

    expect(nodeA).toBeDefined();
    expect(nodeB).toBeDefined();
    expect(nodeC).toBeDefined();
    expect(nodeOutput).toBeDefined();

    expect(nodeA!.nextNodes).toEqual(['c']);
    expect(nodeA!.prevNodes).toEqual([]);

    expect(nodeB!.nextNodes).toEqual(['c']);
    expect(nodeB!.prevNodes).toEqual([]);

    expect(nodeC!.nextNodes).toEqual(['output']);
    expect(nodeC!.prevNodes).toEqual(['a', 'b']);

    expect(nodeOutput!.prevNodes).toEqual(['c']);
  });

  it('should handle a node referencing a previously referenced node', () => {
    const sql = `
      WITH a AS (SELECT 1 as id),
           b AS (SELECT * FROM a),
           c AS (SELECT * FROM a),
           d AS (SELECT * FROM b JOIN c ON b.id = c.id)
      SELECT * FROM d
    `;
    const graphJson = createGraphFromSql(sql);
    const graph: SerializedGraph = JSON.parse(graphJson);

    expect(graph.nodes.length).toBe(5);
    expect(graph.rootNodeIds).toEqual(['a']);

    const nodeA = graph.nodes.find((n) => n.nodeId === 'a');
    const nodeB = graph.nodes.find((n) => n.nodeId === 'b');
    const nodeC = graph.nodes.find((n) => n.nodeId === 'c');
    const nodeD = graph.nodes.find((n) => n.nodeId === 'd');
    const nodeOutput = graph.nodes.find((n) => n.nodeId === 'output');

    expect(nodeA).toBeDefined();
    expect(nodeB).toBeDefined();
    expect(nodeC).toBeDefined();
    expect(nodeD).toBeDefined();
    expect(nodeOutput).toBeDefined();

    expect(nodeA!.nextNodes).toEqual(['b', 'c']);
    expect(nodeA!.prevNodes).toEqual([]);

    expect(nodeB!.nextNodes).toEqual(['d']);
    expect(nodeB!.prevNodes).toEqual(['a']);

    expect(nodeC!.nextNodes).toEqual(['d']);
    expect(nodeC!.prevNodes).toEqual(['a']);

    expect(nodeD!.nextNodes).toEqual(['output']);
    expect(nodeD!.prevNodes).toEqual(['b', 'c']);

    expect(nodeOutput!.prevNodes).toEqual(['d']);
  });
});
