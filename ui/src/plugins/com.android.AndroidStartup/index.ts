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

import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import type {Trace} from '../../public/trace';
import type {PerfettoPlugin} from '../../public/plugin';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';
import {optimizationsTrack} from './optimizations';
import {Time} from '../../base/time';
import type {App} from '../../public/app';
import type {RouteArgs} from '../../public/route_schema';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {StartupDetailsPanel} from './details_panel';
import {findMainThreadTrackUri, scrollToTrackAndSelect} from './navigate';

const STARTUP_TRACK_URI = '/android_startups';
const BREAKDOWN_TRACK_URI = '/android_startups_breakdown';

interface StartupArgs {
  packageName?: string;
  startupId?: number;
  autoSelect?: boolean; // true if the base plugin id 'com.android.AndroidStartup' is present in the route args
}

function getStartupArgsFromRouteArgs(args: RouteArgs): StartupArgs {
  const tempArgs: StartupArgs = {autoSelect: false};

  const baseKey = AndroidStartup.id;
  const packageNameKey = baseKey + '.packageName';
  const startupIdKey = baseKey + '.startupId';

  const packageName = args[packageNameKey];
  if (typeof packageName === 'string') {
    tempArgs.packageName = packageName;
  }

  const startupId = args[startupIdKey];
  if (typeof startupId === 'string') {
    const numStartupId = Number(startupId);
    if (!isNaN(numStartupId) && Number.isInteger(numStartupId)) {
      tempArgs.startupId = numStartupId;
    }
  }

  // Default behaviour: if the flag '${AndroidStartup.id}' is the ONLY argument
  // then auto-select the last startup.
  if (args.hasOwnProperty(baseKey)) {
    tempArgs.autoSelect = true;
  }

  return tempArgs;
}

let startupArgs: StartupArgs;

export default class AndroidStartup implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidStartup';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  static onActivate(app: App): void {
    const args: RouteArgs = app.initialRouteArgs;
    startupArgs = getStartupArgsFromRouteArgs(args);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(`
      include perfetto module android.startup.startups;
    `);

    const cnt = await e.query('select count() cnt from android_startups');
    if (cnt.firstRow({cnt: LONG}).cnt === 0n) {
      return;
    }

    await e.query(`
      include perfetto module android.startup.startup_breakdowns;
    `);

    ctx.tracks.registerTrack({
      uri: STARTUP_TRACK_URI,
      renderer: await SliceTrack.createMaterialized({
        trace: ctx,
        uri: STARTUP_TRACK_URI,
        dataset: new SourceDataset({
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
            startup_type: STR_NULL,
            upid: NUM_NULL,
          },
          src: `
            SELECT
              s.startup_id AS id,
              s.ts,
              s.dur,
              s.package AS name,
              s.startup_type,
              (
                SELECT p.upid
                FROM android_startup_processes p
                WHERE p.startup_id = s.startup_id
                LIMIT 1
              ) AS upid
            FROM android_startups s
          `,
        }),
        detailsPanel: () => new StartupDetailsPanel(ctx),
      }),
    });

    // Needs a sort order lower than 'Ftrace Events' so that it is prioritized in the UI.
    const startupTrack = new TrackNode({
      name: 'Android App Startups',
      uri: STARTUP_TRACK_URI,
      sortOrder: -6,
    });
    ctx.defaultWorkspace.addChildInOrder(startupTrack);

    ctx.tracks.registerTrack({
      uri: BREAKDOWN_TRACK_URI,
      renderer: await SliceTrack.createMaterialized({
        trace: ctx,
        uri: BREAKDOWN_TRACK_URI,
        dataset: new SourceDataset({
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
          },
          src: `
            SELECT
              ts,
              dur,
              reason AS name
            FROM android_startup_opinionated_breakdown
          `,
        }),
      }),
    });

    // Needs a sort order lower than 'Ftrace Events' so that it is prioritized in the UI.
    const breakdownTrack = new TrackNode({
      name: 'Android App Startups Breakdown',
      uri: BREAKDOWN_TRACK_URI,
      sortOrder: -6,
    });
    startupTrack.addChildLast(breakdownTrack);

    const optimizations = await optimizationsTrack(ctx);
    if (optimizations) {
      startupTrack.addChildLast(optimizations);
    }

    await this.selectStartupMainThread(ctx, startupArgs);
  }

  private async selectStartupMainThread(ctx: Trace, args: StartupArgs) {
    const e = ctx.engine;

    const whereFilters = [];
    if (args.packageName !== undefined) {
      whereFilters.push(`s.package = '${args.packageName}'`);
    }
    if (args.startupId !== undefined) {
      whereFilters.push(`s.startup_id = ${args.startupId}`);
    }

    // Order by descending ts to get the last startup first
    const orderByClause = 'ORDER BY s.ts DESC';
    let whereClause = '';

    if (whereFilters.length > 0) {
      whereClause =
        'WHERE ' + whereFilters.join(' AND ') + ' AND t.is_main_thread = 1';
    } else if (args.autoSelect) {
      whereClause = 'WHERE t.is_main_thread = 1';
    } else {
      return;
    }

    const query = `
      SELECT
        s.ts,
        s.dur,
        p.upid,
        tt.id AS main_thread_track_id
      FROM
        android_startups s
      JOIN
        android_startup_processes p ON s.startup_id = p.startup_id
      JOIN
        thread t ON p.upid = t.upid
      JOIN
        thread_track tt ON t.utid = tt.utid
      ${whereClause}
      ${orderByClause}
      LIMIT 1;
    `;

    const result = await e.query(query);
    const it = result.iter({
      ts: LONG,
      dur: LONG_NULL,
      upid: NUM,
      main_thread_track_id: NUM,
    });
    if (!it.valid()) {
      return;
    }

    const startupInfo = {
      ts: it.ts,
      dur: it.dur ?? 0n,
      upid: it.upid,
      mainThreadTrackId: it.main_thread_track_id,
    };

    // Pin the Android Startups track first.
    const trackNode = ctx.currentWorkspace.getTrackByUri(STARTUP_TRACK_URI);
    if (trackNode) {
      trackNode.pin();
    }

    const startTime = Time.fromRaw(BigInt(startupInfo.ts));

    ctx.onTraceReady.addListener(async () => {
      const group = (
        ctx.plugins.getPlugin(
          ProcessThreadGroupsPlugin,
        ) as ProcessThreadGroupsPlugin
      ).getGroupForProcess(startupInfo.upid);

      if (!group?.uri) return;
      group.expand();

      const tracksToSelect: string[] = [];
      const mainThreadTrackUri = findMainThreadTrackUri(
        ctx,
        startupInfo.mainThreadTrackId,
      );
      if (mainThreadTrackUri) {
        tracksToSelect.push(mainThreadTrackUri);
      }

      scrollToTrackAndSelect(
        ctx,
        group.uri,
        tracksToSelect,
        startTime,
        startupInfo.dur,
      );
    });
  }
}
