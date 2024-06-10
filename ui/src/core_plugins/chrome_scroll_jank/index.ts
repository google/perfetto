// Copyright (C) 2022 The Android Open Source Project
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

import {v4 as uuidv4} from 'uuid';

import {uuidv4Sql} from '../../base/uuid';
import {Actions, DeferredAction} from '../../common/actions';
import {generateSqlWithInternalLayout} from '../../common/internal_layout_utils';
import {featureFlags} from '../../core/feature_flags';
import {GenericSliceDetailsTabConfig} from '../../frontend/generic_slice_details_tab';
import {
  BottomTabToSCSAdapter,
  NUM,
  Plugin,
  PluginContextTrace,
  PluginDescriptor,
} from '../../public';
import {Engine} from '../../trace_processor/engine';

import {ChromeTasksScrollJankTrack} from './chrome_tasks_scroll_jank_track';
import {
  CHROME_EVENT_LATENCY_TRACK_KIND,
  DecideTracksResult,
  ENABLE_CHROME_SCROLL_JANK_PLUGIN,
  SCROLL_JANK_GROUP_ID,
  ScrollJankV3TrackKind,
} from './common';
import {EventLatencySliceDetailsPanel} from './event_latency_details_panel';
import {
  addLatencyTracks,
  EventLatencyTrack,
  JANKY_LATENCY_NAME,
} from './event_latency_track';
import {ScrollDetailsPanel} from './scroll_details_panel';
import {ScrollJankCauseMap} from './scroll_jank_cause_map';
import {ScrollJankV3DetailsPanel} from './scroll_jank_v3_details_panel';
import {
  addScrollJankV3ScrollTrack,
  ScrollJankV3Track,
} from './scroll_jank_v3_track';
import {
  addTopLevelScrollTrack,
  CHROME_TOPLEVEL_SCROLLS_KIND,
  TopLevelScrollTrack,
} from './scroll_track';

export const ENABLE_SCROLL_JANK_PLUGIN_V2 = featureFlags.register({
  id: 'enableScrollJankPluginV2',
  name: 'Enable Scroll Jank plugin V2',
  description: 'Adds new tracks and visualizations for scroll jank.',
  defaultValue: false,
});

export type ScrollJankTrackGroup = {
  tracks: DecideTracksResult;
  addTrackGroup: DeferredAction;
};

export async function getScrollJankTracks(
  engine: Engine,
): Promise<ScrollJankTrackGroup> {
  const result: DecideTracksResult = {
    tracksToAdd: [],
  };

  const scrolls = await addTopLevelScrollTrack();
  result.tracksToAdd = result.tracksToAdd.concat(scrolls.tracksToAdd);

  const janks = await addScrollJankV3ScrollTrack();
  result.tracksToAdd = result.tracksToAdd.concat(janks.tracksToAdd);

  const eventLatencies = await addLatencyTracks();
  result.tracksToAdd = result.tracksToAdd.concat(eventLatencies.tracksToAdd);

  const addTrackGroup = Actions.addTrackGroup({
    name: 'Chrome Scroll Jank',
    key: SCROLL_JANK_GROUP_ID,
    collapsed: false,
    fixedOrdering: true,
  });

  await ScrollJankCauseMap.initialize(engine);
  return {tracks: result, addTrackGroup};
}

class ChromeScrollJankPlugin implements Plugin {
  async onTraceLoad(ctx: PluginContextTrace): Promise<void> {
    await this.addChromeScrollJankTrack(ctx);
    await this.addTopLevelScrollTrack(ctx);
    await this.addEventLatencyTrack(ctx);
    await this.addScrollJankV3ScrollTrack(ctx);

    if (!ENABLE_CHROME_SCROLL_JANK_PLUGIN.get()) {
      return;
    }

    if (!(await isChromeTrace(ctx.engine))) {
      return;
    }

    // Initialise the chrome_tasks_delaying_input_processing table. It will be
    // used in the tracks above.
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE deprecated.v42.common.slices;
      SELECT RUN_METRIC(
        'chrome/chrome_tasks_delaying_input_processing.sql',
        'duration_causing_jank_ms',
        /* duration_causing_jank_ms = */ '8');`);

    const query = `
       select
         s1.full_name,
         s1.duration_ms,
         s1.slice_id,
         s1.thread_dur_ms,
         s2.id,
         s2.ts,
         s2.dur,
         s2.track_id
       from chrome_tasks_delaying_input_processing s1
       join slice s2 on s1.slice_id=s2.id
       `;
    ctx.tabs.openQuery(query, 'Scroll Jank: long tasks');
  }

  private async addChromeScrollJankTrack(
    ctx: PluginContextTrace,
  ): Promise<void> {
    ctx.registerTrack({
      uri: 'perfetto.ChromeScrollJank',
      displayName: 'Scroll Jank causes - long tasks',
      kind: ChromeTasksScrollJankTrack.kind,
      trackFactory: ({trackKey}) => {
        return new ChromeTasksScrollJankTrack({
          engine: ctx.engine,
          trackKey,
        });
      },
    });
  }

  private async addTopLevelScrollTrack(ctx: PluginContextTrace): Promise<void> {
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE chrome.chrome_scrolls;
      INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_offsets;
    `);

    ctx.registerTrack({
      uri: 'perfetto.ChromeScrollJank#toplevelScrolls',
      displayName: 'Chrome Scrolls',
      kind: CHROME_TOPLEVEL_SCROLLS_KIND,
      trackFactory: ({trackKey}) => {
        return new TopLevelScrollTrack({
          engine: ctx.engine,
          trackKey,
        });
      },
    });

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (selection) => {
          if (
            selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind === ScrollDetailsPanel.kind
          ) {
            const config = selection.detailsPanelConfig.config;
            return new ScrollDetailsPanel({
              config: config as GenericSliceDetailsTabConfig,
              engine: ctx.engine,
              uuid: uuidv4(),
            });
          }
          return undefined;
        },
      }),
    );
  }

  private async addEventLatencyTrack(ctx: PluginContextTrace): Promise<void> {
    const subTableSql = generateSqlWithInternalLayout({
      columns: ['id', 'ts', 'dur', 'track_id', 'name'],
      sourceTable: 'slice',
      ts: 'ts',
      dur: 'dur',
      whereClause: `
        EXTRACT_ARG(arg_set_id, 'event_latency.event_type') IN (
          'FIRST_GESTURE_SCROLL_UPDATE',
          'GESTURE_SCROLL_UPDATE',
          'INERTIAL_GESTURE_SCROLL_UPDATE')
        AND has_descendant_slice_with_name(
          id,
          'SubmitCompositorFrameToPresentationCompositorFrame')
        AND name = 'EventLatency'
        AND depth = 0`,
    });

    // Table name must be unique - it cannot include '-' characters or begin
    // with a numeric value.
    const baseTable = `table_${uuidv4Sql()}_janky_event_latencies_v3`;
    const tableDefSql = `CREATE TABLE ${baseTable} AS
        WITH
        event_latencies AS MATERIALIZED (
          ${subTableSql}
        ),
        latency_stages AS (
          SELECT
            stage.id,
            stage.ts,
            stage.dur,
            stage.track_id,
            stage.name,
            stage.depth,
            event.id as event_latency_id,
            event.depth as event_latency_depth
          FROM event_latencies event
          JOIN descendant_slice(event.id) stage
          UNION ALL
          SELECT
            event.id,
            event.ts,
            event.dur,
            event.track_id,
            IIF(
              id IN (SELECT id FROM chrome_janky_event_latencies_v3),
              '${JANKY_LATENCY_NAME}',
              name
            ) as name,
            0 as depth,
            event.id as event_latency_id,
            event.depth as event_latency_depth
          FROM event_latencies event
        ),
        -- Event latencies have already had layout computed, but the width of event latency can vary (3 or 4),
        -- so we have to compute the max stage depth for each event latency depth to compute offset for each
        -- event latency row.
        event_latency_height_per_row AS (
          SELECT
            event_latency_depth,
            MAX(depth) AS max_depth
          FROM latency_stages
          GROUP BY event_latency_depth
        ),
        -- Compute the offset for each event latency depth using max depth info for each depth.
        event_latency_layout_offset AS (
          SELECT
            event_latency_depth,
            -- As the sum is exclusive, it will return NULL for the first row — we need to set it to 0 explicitly.
            IFNULL(
              SUM(max_depth + 1) OVER (
                ORDER BY event_latency_depth
                ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING
              ),
            0) as offset
          FROM event_latency_height_per_row
        )
      SELECT
        stage.id,
        stage.ts,
        stage.dur,
        stage.name,
        stage.depth + (
          (
            SELECT offset.offset
            FROM event_latencies event
            JOIN event_latency_layout_offset offset ON event.depth = offset.event_latency_depth
            WHERE id = stage.event_latency_id
          )
        ) AS depth
      FROM latency_stages stage;`;

    await ctx.engine.query(
      `INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_intervals`,
    );
    await ctx.engine.query(tableDefSql);

    ctx.registerTrack({
      uri: 'perfetto.ChromeScrollJank#eventLatency',
      displayName: 'Chrome Scroll Input Latencies',
      kind: CHROME_EVENT_LATENCY_TRACK_KIND,
      trackFactory: ({trackKey}) => {
        return new EventLatencyTrack({engine: ctx.engine, trackKey}, baseTable);
      },
    });

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (selection) => {
          if (
            selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind ===
              EventLatencySliceDetailsPanel.kind
          ) {
            const config = selection.detailsPanelConfig.config;
            return new EventLatencySliceDetailsPanel({
              config: config as GenericSliceDetailsTabConfig,
              engine: ctx.engine,
              uuid: uuidv4(),
            });
          }
          return undefined;
        },
      }),
    );
  }

  private async addScrollJankV3ScrollTrack(
    ctx: PluginContextTrace,
  ): Promise<void> {
    await ctx.engine.query(
      `INCLUDE PERFETTO MODULE chrome.scroll_jank.scroll_jank_intervals`,
    );

    ctx.registerTrack({
      uri: 'perfetto.ChromeScrollJank#scrollJankV3',
      displayName: 'Chrome Scroll Janks',
      kind: ScrollJankV3TrackKind,
      trackFactory: ({trackKey}) => {
        return new ScrollJankV3Track({
          engine: ctx.engine,
          trackKey,
        });
      },
    });

    ctx.registerDetailsPanel(
      new BottomTabToSCSAdapter({
        tabFactory: (selection) => {
          if (
            selection.kind === 'GENERIC_SLICE' &&
            selection.detailsPanelConfig.kind === ScrollJankV3DetailsPanel.kind
          ) {
            const config = selection.detailsPanelConfig.config;
            return new ScrollJankV3DetailsPanel({
              config: config as GenericSliceDetailsTabConfig,
              engine: ctx.engine,
              uuid: uuidv4(),
            });
          }
          return undefined;
        },
      }),
    );
  }
}

async function isChromeTrace(engine: Engine) {
  const queryResult = await engine.query(`
      select utid, upid
      from thread
      where name='CrBrowserMain'
      `);

  const it = queryResult.iter({
    utid: NUM,
    upid: NUM,
  });

  return it.valid();
}

export const plugin: PluginDescriptor = {
  pluginId: 'perfetto.ChromeScrollJank',
  plugin: ChromeScrollJankPlugin,
};
