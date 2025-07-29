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

import {getThreadInfo, ThreadInfo} from '../../components/sql_utils/thread';
import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';
import {Trace} from '../../public/trace';
import {THREAD_STATE_TRACK_KIND} from '../../public/track_kinds';
import {PerfettoPlugin} from '../../public/plugin';
import {asUtid, Utid} from '../../components/sql_utils/core_types';
import {addQueryResultsTab} from '../../components/query_table/query_result_tab';
import {showModal} from '../../widgets/modal';
import {
  CRITICAL_PATH_CMD,
  CRITICAL_PATH_LITE_CMD,
} from '../../public/exposed_commands';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {NUM} from '../../trace_processor/query_result';

const criticalPathSliceColumns = {
  ts: 'ts',
  dur: 'dur',
  name: 'name',
};

const criticalPathsliceColumnNames = [
  'id',
  'utid',
  'ts',
  'dur',
  'name',
  'table_name',
];

const criticalPathsliceLiteColumns = {
  ts: 'ts',
  dur: 'dur',
  name: 'thread_name',
};

const criticalPathsliceLiteColumnNames = [
  'id',
  'utid',
  'ts',
  'dur',
  'thread_name',
  'process_name',
  'table_name',
];

const sliceLiteColumns = {ts: 'ts', dur: 'dur', name: 'thread_name'};

const sliceLiteColumnNames = [
  'id',
  'utid',
  'ts',
  'dur',
  'thread_name',
  'process_name',
  'table_name',
];

const sliceColumns = {ts: 'ts', dur: 'dur', name: 'name'};

const sliceColumnNames = ['id', 'utid', 'ts', 'dur', 'name', 'table_name'];

function getFirstUtidOfSelectionOrVisibleWindow(trace: Trace): number {
  const selection = trace.selection.selection;
  if (selection.kind === 'area') {
    for (const trackDesc of selection.tracks) {
      if (
        trackDesc?.tags?.kind === THREAD_STATE_TRACK_KIND &&
        trackDesc?.tags?.utid !== undefined
      ) {
        return trackDesc.tags.utid;
      }
    }
  }

  return 0;
}

function showModalErrorAreaSelectionRequired() {
  showModal({
    title: 'Error: range selection required',
    content:
      'This command requires an area selection over a thread state track.',
  });
}

function showModalErrorThreadStateRequired() {
  showModal({
    title: 'Error: thread state selection required',
    content: 'This command requires a thread state slice to be selected.',
  });
}

// If utid is undefined, returns the utid for the selected thread state track,
// if any. If it's defined, looks up the info about that specific utid.
async function getThreadInfoForUtidOrSelection(
  trace: Trace,
  utid?: Utid,
): Promise<ThreadInfo | undefined> {
  const resolvedUtid = utid ?? (await getUtid(trace));
  if (resolvedUtid === undefined) return undefined;
  return await getThreadInfo(trace.engine, resolvedUtid);
}

/**
 * Get the utid for the current selection. We either grab the utid from the
 * track tags, or we look it up from the dataset.
 *
 * Returns undefined if the selection doesn't really have a utid.
 */
async function getUtid(trace: Trace): Promise<Utid | undefined> {
  // No utid passed, look up the utid from the selected track.
  const selection = trace.selection.selection;
  if (selection.kind !== 'track_event') return undefined;

  const trackUri = selection.trackUri;
  const track = trace.tracks.getTrack(trackUri);
  if (track === undefined) return undefined;

  if (
    track.tags &&
    'utid' in track.tags &&
    typeof track.tags.utid === 'number'
  ) {
    return asUtid(track.tags.utid);
  }

  const dataset = track.renderer.getDataset?.();
  if (dataset === undefined) return undefined;
  if (!dataset.implements({utid: NUM})) return undefined;

  const result = await trace.engine.query(`
    SELECT utid FROM (${dataset.query()}) WHERE id = ${selection.eventId}
  `);
  const firstRow = result.firstRow({utid: NUM});
  return asUtid(firstRow?.utid);
}

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.CriticalPath';
  async onTraceLoad(ctx: Trace): Promise<void> {
    // The 3 commands below are used in two contextes:
    // 1. By clicking a slice and using the command palette. In this case the
    //    utid argument is undefined and we need to look at the selection.
    // 2. Invoked via runCommand(...) by thread_state_tab.ts when the user
    //    clicks on the buttons in the details panel. In this case the details
    //    panel passes the utid explicitly.
    ctx.commands.registerCommand({
      id: CRITICAL_PATH_LITE_CMD,
      name: 'Critical path lite (selected thread state slice)',
      callback: async (utid?: Utid) => {
        const thdInfo = await getThreadInfoForUtidOrSelection(ctx, utid);
        if (thdInfo === undefined) {
          return showModalErrorThreadStateRequired();
        }
        ctx.engine
          .query(`INCLUDE PERFETTO MODULE sched.thread_executing_span;`)
          .then(() =>
            addDebugSliceTrack({
              trace: ctx,
              data: {
                sqlSource: `
                SELECT
                  cr.id,
                  cr.utid,
                  cr.ts,
                  cr.dur,
                  thread.name AS thread_name,
                  process.name AS process_name,
                  'thread_state' AS table_name
                FROM
                  _thread_executing_span_critical_path(
                    ${thdInfo.utid},
                    trace_bounds.start_ts,
                    trace_bounds.end_ts - trace_bounds.start_ts) cr,
                  trace_bounds
                JOIN thread USING(utid)
                JOIN process USING(upid)
              `,
                columns: sliceLiteColumnNames,
              },
              title: `${thdInfo.name}`,
              columns: sliceLiteColumns,
              argColumns: sliceLiteColumnNames,
            }),
          );
      },
    });

    ctx.commands.registerCommand({
      id: CRITICAL_PATH_CMD,
      name: 'Critical path (selected thread state slice)',
      callback: async (utid?: Utid) => {
        const thdInfo = await getThreadInfoForUtidOrSelection(ctx, utid);
        if (thdInfo === undefined) {
          return showModalErrorThreadStateRequired();
        }
        ctx.engine
          .query(
            `INCLUDE PERFETTO MODULE sched.thread_executing_span_with_slice;`,
          )
          .then(() =>
            addDebugSliceTrack({
              trace: ctx,
              data: {
                sqlSource: `
                SELECT cr.id, cr.utid, cr.ts, cr.dur, cr.name, cr.table_name
                  FROM
                    _thread_executing_span_critical_path_stack(
                      ${thdInfo.utid},
                      trace_bounds.start_ts,
                      trace_bounds.end_ts - trace_bounds.start_ts) cr,
                    trace_bounds WHERE name IS NOT NULL
              `,
                columns: sliceColumnNames,
              },
              title: `${thdInfo.name}`,
              columns: sliceColumns,
              argColumns: sliceColumnNames,
            }),
          );
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.CriticalPathLite_AreaSelection',
      name: 'Critical path lite (over area selection)',
      callback: async () => {
        const trackUtid = getFirstUtidOfSelectionOrVisibleWindow(ctx);
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        if (trackUtid === 0) {
          return showModalErrorAreaSelectionRequired();
        }
        await ctx.engine.query(
          `INCLUDE PERFETTO MODULE sched.thread_executing_span;`,
        );
        await addDebugSliceTrack({
          trace: ctx,
          data: {
            sqlSource: `
                SELECT
                  cr.id,
                  cr.utid,
                  cr.ts,
                  cr.dur,
                  thread.name AS thread_name,
                  process.name AS process_name,
                  'thread_state' AS table_name
                FROM
                  _thread_executing_span_critical_path(
                      ${trackUtid},
                      ${window.start},
                      ${window.end} - ${window.start}) cr
                JOIN thread USING(utid)
                JOIN process USING(upid)
                `,
            columns: criticalPathsliceLiteColumnNames,
          },
          title:
            (await getThreadInfo(ctx.engine, trackUtid as Utid)).name ??
            '<thread name>',
          columns: criticalPathsliceLiteColumns,
          argColumns: criticalPathsliceLiteColumnNames,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.CriticalPath_AreaSelection',
      name: 'Critical path  (over area selection)',
      callback: async () => {
        const trackUtid = getFirstUtidOfSelectionOrVisibleWindow(ctx);
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        if (trackUtid === 0) {
          return showModalErrorAreaSelectionRequired();
        }
        await ctx.engine.query(
          `INCLUDE PERFETTO MODULE sched.thread_executing_span_with_slice;`,
        );
        await addDebugSliceTrack({
          trace: ctx,
          data: {
            sqlSource: `
                SELECT cr.id, cr.utid, cr.ts, cr.dur, cr.name, cr.table_name
                FROM
                _critical_path_stack(
                  ${trackUtid},
                  ${window.start},
                  ${window.end} - ${window.start}, 1, 1, 1, 1) cr
                WHERE name IS NOT NULL
                `,
            columns: criticalPathsliceColumnNames,
          },
          title:
            (await getThreadInfo(ctx.engine, trackUtid as Utid)).name ??
            '<thread name>',
          columns: criticalPathSliceColumns,
          argColumns: criticalPathsliceColumnNames,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'perfetto.CriticalPathPprof_AreaSelection',
      name: 'Critical path pprof (over area selection)',
      callback: async () => {
        const trackUtid = getFirstUtidOfSelectionOrVisibleWindow(ctx);
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        if (trackUtid === 0) {
          return showModalErrorAreaSelectionRequired();
        }
        addQueryResultsTab(ctx, {
          query: `
              INCLUDE PERFETTO MODULE sched.thread_executing_span_with_slice;
              SELECT *
                FROM
                  _thread_executing_span_critical_path_graph(
                  "criical_path",
                    ${trackUtid},
                    ${window.start},
                    ${window.end} - ${window.start}) cr`,
          title: 'Critical path',
        });
      },
    });
  }
}
