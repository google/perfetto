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

import {createPerfettoTable} from '../../trace_processor/sql_utils';
import {generateSqlWithInternalLayout} from '../../components/sql_utils/layout';
import {
  NAMED_ROW,
  NamedRow,
  NamedSliceTrack,
} from '../../components/tracks/named_slice_track';
import {Slice} from '../../public/track';
import {sqlNameSafe} from '../../base/string_utils';
import {SqlTableSliceTrackDetailsPanel} from '../../components/tracks/sql_table_slice_track_details_tab';
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';

interface StepTemplate {
  step_name: string;
  ts_column_name: string;
  dur_column_name: string;
}

// TODO: b/383547343 - Migrate STEP_TEMPLATES to a Chrome tracing stdlib table
// once it's stable.
const STEP_TEMPLATES: readonly StepTemplate[] = [
  {
    step_name: 'GenerationToBrowserMain',
    ts_column_name: 'generation_ts',
    dur_column_name: 'generation_to_browser_main_dur',
  },
  {
    step_name: 'TouchMoveProcessing',
    ts_column_name: 'touch_move_received_ts',
    dur_column_name: 'touch_move_processing_dur',
  },
  {
    step_name: 'ScrollUpdateProcessing',
    ts_column_name: 'scroll_update_created_ts',
    dur_column_name: 'scroll_update_processing_dur',
  },
  {
    step_name: 'BrowserMainToRendererCompositor',
    ts_column_name: 'scroll_update_created_end_ts',
    dur_column_name: 'browser_to_compositor_delay_dur',
  },
  {
    step_name: 'RendererCompositorDispatch',
    ts_column_name: 'compositor_dispatch_ts',
    dur_column_name: 'compositor_dispatch_dur',
  },
  {
    step_name: 'RendererCompositorDispatchToOnBeginFrame',
    ts_column_name: 'compositor_dispatch_end_ts',
    dur_column_name: 'compositor_dispatch_to_on_begin_frame_delay_dur',
  },
  {
    step_name: 'RendererCompositorBeginFrame',
    ts_column_name: 'compositor_on_begin_frame_ts',
    dur_column_name: 'compositor_on_begin_frame_dur',
  },
  {
    step_name: 'RendererCompositorBeginToGenerateFrame',
    ts_column_name: 'compositor_on_begin_frame_end_ts',
    dur_column_name: 'compositor_on_begin_frame_to_generation_delay_dur',
  },
  {
    step_name: 'RendererCompositorGenerateToSubmitFrame',
    ts_column_name: 'compositor_generate_compositor_frame_ts',
    dur_column_name: 'compositor_generate_frame_to_submit_frame_dur',
  },
  {
    step_name: 'RendererCompositorSubmitFrame',
    ts_column_name: 'compositor_submit_compositor_frame_ts',
    dur_column_name: 'compositor_submit_frame_dur',
  },
  {
    step_name: 'RendererCompositorToViz',
    ts_column_name: 'compositor_submit_compositor_frame_end_ts',
    dur_column_name: 'compositor_to_viz_delay_dur',
  },
  {
    step_name: 'VizReceiveFrame',
    ts_column_name: 'viz_receive_compositor_frame_ts',
    dur_column_name: 'viz_receive_compositor_frame_dur',
  },
  {
    step_name: 'VizReceiveToDrawFrame',
    ts_column_name: 'viz_receive_compositor_frame_end_ts',
    dur_column_name: 'viz_wait_for_draw_dur',
  },
  {
    step_name: 'VizDrawToSwapFrame',
    ts_column_name: 'viz_draw_and_swap_ts',
    dur_column_name: 'viz_draw_and_swap_dur',
  },
  {
    step_name: 'VizToGpu',
    ts_column_name: 'viz_send_buffer_swap_end_ts',
    dur_column_name: 'viz_to_gpu_delay_dur',
  },
  {
    step_name: 'VizSwapBuffers',
    ts_column_name: 'viz_swap_buffers_ts',
    dur_column_name: 'viz_swap_buffers_dur',
  },
  {
    step_name: 'VizSwapBuffersToLatch',
    ts_column_name: 'viz_swap_buffers_end_ts',
    dur_column_name: 'viz_swap_buffers_to_latch_dur',
  },
  {
    step_name: 'VizLatchToSwapEnd',
    ts_column_name: 'latch_timestamp',
    dur_column_name: 'viz_latch_to_swap_end_dur',
  },
  {
    step_name: 'VizSwapEndToPresentation',
    ts_column_name: 'swap_end_timestamp',
    dur_column_name: 'swap_end_to_presentation_dur',
  },
  {
    // An artificial step to ensure that presentation_timestamp is included in
    // the calculation of scroll_update_bounds. It's filtered out in
    // unordered_slices due to NULL duration.
    step_name: '',
    ts_column_name: 'presentation_timestamp',
    dur_column_name: 'NULL',
  },
];

export class ScrollTimelineTrack extends NamedSliceTrack<Slice, NamedRow> {
  private readonly tableName;

  constructor(trace: Trace, uri: string) {
    super(trace, uri);
    this.tableName = `scrolltimelinetrack_${sqlNameSafe(uri)}`;
  }
  override async onInit(): Promise<AsyncDisposable> {
    await super.onInit();
    await this.engine.query(`INCLUDE PERFETTO MODULE chrome.chrome_scrolls;`);
    // TODO: b/383549233 - Set ts+dur of each scroll update directly based on
    // our knowledge of the scrolling pipeline (as opposed to aggregating over
    // scroll_steps).
    return await createPerfettoTable(
      this.engine,
      this.tableName,
      `WITH
        -- Unpivot all ts+dur columns into rows. Each row corresponds to a step
        -- of a particular scroll update. Some of the rows might have null
        -- ts/dur values, which will be filtered out in unordered_slices.
        -- |scroll_steps| = |chrome_scroll_update_info| * |STEP_TEMPLATES|
        scroll_steps AS (${STEP_TEMPLATES.map(
          (step) => `
          SELECT
            id AS scroll_id,
            ${step.ts_column_name} AS ts,
            ${step.dur_column_name} AS dur,
            '${step.step_name}' AS name
          FROM chrome_scroll_update_info`,
        ).join(' UNION ALL ')}),
        -- For each scroll update, find its ts+dur by aggregating over all steps
        -- within the scroll update. We're basically trying to find MIN(COL1_ts,
        -- COL2_ts, ..., COLn_ts) and MAX(COL1_ts, COL2_ts, ..., COLn_ts) from
        -- all the various ts columns in chrome_scroll_update_info. The
        -- difficulty is that some of those columns might be null, which is
        -- better handled by the aggregate MIN/MAX functions (which ignore null
        -- values) than the scalar MIN/MAX functions (which return null if any
        -- argument is null). That's why we do it in such a roundabout way by
        -- joining the top-level table with the individual steps.
        scroll_update_bounds AS (
          SELECT
            scroll_update.id AS scroll_id,
            MIN(scroll_steps.ts) AS ts,
            MAX(scroll_steps.ts) - MIN(scroll_steps.ts) AS dur
          FROM
            chrome_scroll_update_info AS scroll_update
            JOIN scroll_steps ON scroll_steps.scroll_id = scroll_update.id
          GROUP BY scroll_update.id
        ),
        -- Now that we know the ts+dur of all scroll updates, we can lay them
        -- out efficiently (i.e. assign depths to them to avoid overlaps).
        scroll_update_layouts AS (
          ${generateSqlWithInternalLayout({
            columns: ['scroll_id', 'ts', 'dur'],
            sourceTable: 'scroll_update_bounds',
            ts: 'ts',
            dur: 'dur',
          })}
        ),
        -- We interleave the top-level scroll update slices (at even depths) and
        -- their constituent step slices (at odd depths).
        unordered_slices AS (
          SELECT
            ts,
            dur,
            2 * depth AS depth,
            'Scroll Update' AS name
          FROM scroll_update_layouts
          UNION ALL
          SELECT
            scroll_steps.ts,
            MAX(scroll_steps.dur, 0) AS dur,
            2 * scroll_update_layouts.depth + 1 AS depth,
            scroll_steps.name
          FROM scroll_steps
          JOIN scroll_update_layouts USING(scroll_id)
          WHERE scroll_steps.ts IS NOT NULL AND scroll_steps.dur IS NOT NULL
        )
      -- Finally, we sort all slices chronologically and assign them
      -- monotonically increasing IDs. Note that we cannot reuse
      -- chrome_scroll_update_info.id (not even for the top-level scroll update
      -- slices) because Perfetto slice IDs must be 32-bit unsigned integers.
      SELECT
        ROW_NUMBER() OVER (ORDER BY ts ASC) AS id,
        *
      FROM unordered_slices
      ORDER BY ts ASC`,
    );
  }

  override getSqlSource(): string {
    return `SELECT * FROM ${this.tableName}`;
  }

  override getRowSpec(): NamedRow {
    return NAMED_ROW;
  }

  override rowToSlice(row: NamedRow): Slice {
    return super.rowToSliceBase(row);
  }

  override detailsPanel(sel: TrackEventSelection): TrackEventDetailsPanel {
    return new SqlTableSliceTrackDetailsPanel(
      this.trace,
      this.tableName,
      sel.eventId,
    );
  }
}
