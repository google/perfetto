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
import QueryPagePlugin from '../dev.perfetto.QueryPage';
import SchedPlugin from '../dev.perfetto.Sched';
import {showModal} from '../../widgets/modal';
import {
  CRITICAL_PATH_CMD,
  CRITICAL_PATH_LITE_CMD,
} from '../../public/exposed_commands';
import {getTimeSpanOfSelectionOrVisibleWindow} from '../../public/utils';
import {LONG, NUM} from '../../trace_processor/query_result';
import {CriticalPathTreePin} from './critical_path_tree';

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
  return getThreadStateTracksInArea(trace)[0]?.utid ?? 0;
}

function getThreadStateTracksInArea(
  trace: Trace,
): ReadonlyArray<{utid: number; uri: string}> {
  const selection = trace.selection.selection;
  if (selection.kind !== 'area') return [];
  const out: {utid: number; uri: string}[] = [];
  for (const trackDesc of selection.tracks) {
    if (
      trackDesc?.tags?.kinds?.includes(THREAD_STATE_TRACK_KIND) &&
      trackDesc?.tags?.utid !== undefined
    ) {
      out.push({utid: trackDesc.tags.utid, uri: trackDesc.uri});
    }
  }
  return out;
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

// URI of the thread state track currently holding the selected slice.
function getSelectedThreadStateTrackUri(trace: Trace): string | undefined {
  const selection = trace.selection.selection;
  if (selection.kind !== 'track_event') return undefined;
  const track = trace.tracks.getTrack(selection.trackUri);
  if (!track?.tags?.kinds?.includes(THREAD_STATE_TRACK_KIND)) return undefined;
  return selection.trackUri;
}

// If utid is undefined, returns the utid for the selected thread state track,
// if any. If it's defined, looks up the info about that specific utid.
async function getThreadInfoForUtidOrSelection(
  trace: Trace,
  utidArg: unknown,
): Promise<ThreadInfo | undefined> {
  const resolvedUtid =
    typeof utidArg === 'number' ? (utidArg as Utid) : await getUtid(trace);
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
  static readonly dependencies = [QueryPagePlugin, SchedPlugin];
  async onTraceLoad(ctx: Trace): Promise<void> {
    // Each command is invoked either from the command palette (utid
    // resolved from the current selection) or via runCommand(utid)
    // from the thread-state details panel.
    ctx.commands.registerCommand({
      id: CRITICAL_PATH_LITE_CMD,
      name: 'Critical path lite (selected thread state slice)',
      callback: async (utidArg) => {
        const thdInfo = await getThreadInfoForUtidOrSelection(ctx, utidArg);
        if (thdInfo === undefined) {
          return showModalErrorThreadStateRequired();
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
                  ${thdInfo.utid},
                  trace_bounds.start_ts,
                  trace_bounds.end_ts - trace_bounds.start_ts) cr,
                trace_bounds
              JOIN thread USING (utid)
              LEFT JOIN process USING (upid)
            `,
            columns: sliceLiteColumnNames,
          },
          title: `${thdInfo.name}`,
          columns: sliceLiteColumns,
          rawColumns: sliceLiteColumnNames,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CriticalPathLite_AreaSelection',
      name: 'Critical path lite (over area selection)',
      callback: async () => {
        const trackUtid = getFirstUtidOfSelectionOrVisibleWindow(ctx);
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        if (trackUtid === 0) return showModalErrorAreaSelectionRequired();
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
              JOIN thread USING (utid)
              LEFT JOIN process USING (upid)
            `,
            columns: sliceLiteColumnNames,
          },
          title:
            (await getThreadInfo(ctx.engine, trackUtid as Utid)).name ??
            '<thread name>',
          columns: sliceLiteColumns,
          rawColumns: sliceLiteColumnNames,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CriticalPathTree',
      name: 'Critical path tree (selected thread state slice)',
      callback: async (utidArg) => {
        const thdInfo = await getThreadInfoForUtidOrSelection(ctx, utidArg);
        if (thdInfo === undefined) return showModalErrorThreadStateRequired();
        const parentUri = getSelectedThreadStateTrackUri(ctx);
        if (parentUri === undefined) return showModalErrorThreadStateRequired();
        const tb = await ctx.engine.query(
          `SELECT start_ts AS s, end_ts AS e FROM trace_bounds`,
        );
        const tbRow = tb.firstRow({s: LONG, e: LONG});
        await new CriticalPathTreePin(
          ctx,
          thdInfo.utid,
          thdInfo.name ?? `utid ${thdInfo.utid}`,
          tbRow.s,
          tbRow.e - tbRow.s,
          parentUri,
        ).pin();
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CriticalPathTree_AreaSelection',
      name: 'Critical path tree (over area selection)',
      callback: async () => {
        const parents = getThreadStateTracksInArea(ctx);
        if (parents.length === 0) return showModalErrorAreaSelectionRequired();
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        for (const parent of parents) {
          const thdInfo = await getThreadInfo(ctx.engine, parent.utid as Utid);
          await new CriticalPathTreePin(
            ctx,
            parent.utid,
            thdInfo.name ?? `utid ${parent.utid}`,
            window.start,
            window.end - window.start,
            parent.uri,
          ).pin();
        }
      },
    });

    ctx.commands.registerCommand({
      id: CRITICAL_PATH_CMD,
      name: 'Critical path stacks (selected thread state slice)',
      callback: async (utidArg) => {
        const thdInfo = await getThreadInfoForUtidOrSelection(ctx, utidArg);
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
              rawColumns: sliceColumnNames,
            }),
          );
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CriticalPath_AreaSelection',
      name: 'Critical path stacks (over area selection)',
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
          rawColumns: criticalPathsliceColumnNames,
        });
      },
    });

    ctx.commands.registerCommand({
      id: 'dev.perfetto.CriticalPathPprof_AreaSelection',
      name: 'Critical path pprof (over area selection)',
      callback: async () => {
        const trackUtid = getFirstUtidOfSelectionOrVisibleWindow(ctx);
        const window = await getTimeSpanOfSelectionOrVisibleWindow(ctx);
        if (trackUtid === 0) {
          return showModalErrorAreaSelectionRequired();
        }
        ctx.plugins.getPlugin(QueryPagePlugin).addQueryResultsTab({
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
