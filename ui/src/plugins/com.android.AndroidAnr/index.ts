// Copyright (C) 2026 The Android Open Source Project
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

import {LONG, LONG_NULL, NUM, STR} from '../../trace_processor/query_result';
import {Trace} from '../../public/trace';
import {PerfettoPlugin} from '../../public/plugin';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {TrackNode} from '../../public/workspace';
import {Time} from '../../base/time';
import {App} from '../../public/app';
import {RouteArgs} from '../../public/route_schema';

const ANR_TRACK_URI = '/android_anrs';

interface AnrArgs {
  processName?: string;
  packageName?: string;
  errorId?: string;
  autoSelect?: boolean; // true if the base plugin id 'com.android.AndroidAnr' is present in the route args
}

function getAnrArgsFromRouteArgs(args: RouteArgs): AnrArgs {
  const tempArgs: AnrArgs = {autoSelect: false};

  const baseKey = AndroidAnr.id;
  const processNameKey = baseKey + '.processName';
  const errorIdKey = baseKey + '.errorId';
  const packageNameKey = baseKey + '.packageName';

  const processName = args[processNameKey];
  if (typeof processName === 'string') {
    tempArgs.processName = processName;
  }

  const packageName = args[packageNameKey];
  if (typeof packageName === 'string') {
    tempArgs.packageName = packageName;
  }

  const errorId = args[errorIdKey];
  if (typeof errorId === 'string') {
    tempArgs.errorId = errorId;
  }

  // Default behaviour: if the flag '${AndroidAnr.id}' is the ONLY argument
  // then auto-select the last ANR.
  if (args.hasOwnProperty(baseKey)) {
    tempArgs.autoSelect = true;
  }

  return tempArgs;
}

let anrArgs: AnrArgs;

export default class AndroidAnr implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidAnr';

  static onActivate(app: App): void {
    const args: RouteArgs = app.initialRouteArgs;
    anrArgs = getAnrArgsFromRouteArgs(args);
  }

  async onTraceLoad(ctx: Trace): Promise<void> {
    const e = ctx.engine;
    await e.query(`
      include perfetto module android.anrs;
    `);

    const cnt = await e.query('select count() cnt from android_anrs');
    if (cnt.firstRow({cnt: LONG}).cnt === 0n) {
      return;
    }

    ctx.tracks.registerTrack({
      uri: ANR_TRACK_URI,
      renderer: await SliceTrack.createMaterialized({
        trace: ctx,
        uri: ANR_TRACK_URI,
        dataset: new SourceDataset({
          schema: {
            ts: LONG,
            dur: LONG_NULL,
            name: STR,
          },
          src: `
            SELECT
              ts - coalesce(anr_dur_ms, default_anr_dur_ms, 0) * 1000000 AS ts,
              coalesce(anr_dur_ms, default_anr_dur_ms, 0) * 1000000 AS dur,
              process_name || ' ' || pid || ' : ' || anr_type AS name
            FROM android_anrs
          `,
        }),
      }),
    });

    // Needs a sort order lower than 'Ftrace Events' so that it is prioritized in the UI.
    const anrTrack = new TrackNode({
      name: 'Android ANRs',
      uri: ANR_TRACK_URI,
      sortOrder: -6,
    });
    ctx.defaultWorkspace.addChildInOrder(anrTrack);

    await this.selectAnrMainThread(ctx, anrArgs);
  }

  private async selectAnrMainThread(ctx: Trace, args: AnrArgs) {
    const e = ctx.engine;

    const whereFilters = [];
    if (args.processName !== undefined) {
      whereFilters.push(`anr.process_name = '${args.processName}'`);
    }
    if (args.packageName !== undefined) {
      whereFilters.push(`apm.package_name = '${args.packageName}'`);
    }
    if (args.errorId !== undefined) {
      whereFilters.push(`anr.error_id = '${args.errorId}'`);
    }

    // Order by descending ts to get the last anr first
    const orderByClause = 'ORDER BY anr.ts DESC';
    let whereClause = '';

    if (whereFilters.length > 0) {
      whereClause =
        'WHERE ' + whereFilters.join(' AND ') + ' AND t.is_main_thread = 1';
    } else if (args.autoSelect) {
      whereClause = 'WHERE t.is_main_thread = 1';
    } else {
      return;
    }

    await e.query(`
      include perfetto module android.process_metadata;
    `);

    const query = `
      SELECT
        anr.ts - coalesce(anr.anr_dur_ms, anr.default_anr_dur_ms) * 1000000 AS ts,
        coalesce(anr.anr_dur_ms, anr.default_anr_dur_ms) * 1000000 AS dur,
        tt.id AS main_thread_track_id
      FROM
        android_anrs anr
      JOIN
        android_process_metadata apm ON anr.upid = apm.upid
      JOIN
        thread t ON anr.upid = t.upid
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
      main_thread_track_id: NUM,
    });
    if (!it.valid()) {
      return;
    }

    const anrInfo = {
      ts: it.ts,
      dur: it.dur ?? 0n, // Default duration to 0 if null
      mainThreadTrackId: it.main_thread_track_id,
    };

    // 1. Pin the Android ANRs track first.
    const trackNode = ctx.currentWorkspace.getTrackByUri(ANR_TRACK_URI);
    if (trackNode) {
      trackNode.pin();
    }

    const startTime = Time.fromRaw(BigInt(anrInfo.ts));
    const endTime = Time.fromRaw(BigInt(anrInfo.ts + anrInfo.dur));

    ctx.onTraceReady.addListener(async () => {
      // Find the main thread track by its track ID via the track tags.
      const mainThreadTrackNode = ctx.currentWorkspace.flatTracks.find(
        (track) => {
          if (!track.uri) {
            return false;
          }
          const trackDesc = ctx.tracks.getTrack(track.uri);
          return trackDesc?.tags?.trackIds?.includes(anrInfo.mainThreadTrackId);
        },
      );

      if (!mainThreadTrackNode?.uri) {
        return;
      }
      const mainThreadTrackUri = mainThreadTrackNode.uri;

      // 2. Scroll to the main thread track and focus into view
      ctx.scrollTo({
        track: {
          uri: mainThreadTrackUri,
          expandGroup: true,
        },
        time:
          anrInfo.dur > 0n
            ? {
                start: startTime,
                end: endTime,
                behavior: {viewPercentage: 0.8},
              }
            : {
                start: startTime,
                behavior: 'focus',
              },
      });

      // 3. Select the area on the main thread track
      ctx.selection.selectArea(
        {
          start: startTime,
          end: endTime,
          trackUris: [mainThreadTrackUri],
        },
        {
          switchToCurrentSelectionTab: true,
        },
      );
    });
  }
}
