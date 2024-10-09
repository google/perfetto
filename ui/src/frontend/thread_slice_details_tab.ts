// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use size file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import m from 'mithril';
import {Icons} from '../base/semantic_icons';
import {Time, TimeSpan} from '../base/time';
import {exists} from '../base/utils';
import {Engine} from '../trace_processor/engine';
import {LONG, LONG_NULL, NUM, STR_NULL} from '../trace_processor/query_result';
import {Button} from '../widgets/button';
import {DetailsShell} from '../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../widgets/grid_layout';
import {MenuItem, PopupMenu2} from '../widgets/menu';
import {Section} from '../widgets/section';
import {Tree} from '../widgets/tree';
import {addDebugSliceTrack} from '../public/lib/debug_tracks/debug_tracks';
import {globals} from './globals';
import {Flow, FlowPoint} from '../core/flow_types';
import {addQueryResultsTab} from '../public/lib/query_table/query_result_tab';
import {hasArgs, renderArguments} from './slice_args';
import {renderDetails} from './slice_details';
import {getSlice, SliceDetails} from '../trace_processor/sql_utils/slice';
import {
  BreakdownByThreadState,
  breakDownIntervalByThreadState,
} from './sql/thread_state';
import {asSliceSqlId} from '../trace_processor/sql_utils/core_types';
import {DurationWidget} from './widgets/duration';
import {addSqlTableTab} from './sql_table_tab_interface';
import {SliceRef} from './widgets/slice';
import {BasicTable} from '../widgets/basic_table';
import {getSqlTableDescription} from './widgets/sql/table/sql_table_registry';
import {assertExists} from '../base/logging';
import {Trace} from '../public/trace';
import {TrackEventDetailsPanel} from '../public/details_panel';
import {TrackEventSelection} from '../public/selection';

interface ContextMenuItem {
  name: string;
  shouldDisplay(slice: SliceDetails): boolean;
  run(slice: SliceDetails, trace: Trace): void;
}

function getTidFromSlice(slice: SliceDetails): number | undefined {
  return slice.thread?.tid;
}

function getPidFromSlice(slice: SliceDetails): number | undefined {
  return slice.process?.pid;
}

function getProcessNameFromSlice(slice: SliceDetails): string | undefined {
  return slice.process?.name;
}

function getThreadNameFromSlice(slice: SliceDetails): string | undefined {
  return slice.thread?.name;
}

function hasName(slice: SliceDetails): boolean {
  return slice.name !== undefined;
}

function hasTid(slice: SliceDetails): boolean {
  return getTidFromSlice(slice) !== undefined;
}

function hasPid(slice: SliceDetails): boolean {
  return getPidFromSlice(slice) !== undefined;
}

function hasProcessName(slice: SliceDetails): boolean {
  return getProcessNameFromSlice(slice) !== undefined;
}

function hasThreadName(slice: SliceDetails): boolean {
  return getThreadNameFromSlice(slice) !== undefined;
}

const ITEMS: ContextMenuItem[] = [
  {
    name: 'Ancestor slices',
    shouldDisplay: (slice: SliceDetails) => slice.parentId !== undefined,
    run: (slice: SliceDetails, trace: Trace) =>
      addSqlTableTab(trace, {
        table: assertExists(getSqlTableDescription('slice')),
        filters: [
          {
            op: (cols) =>
              `${cols[0]} IN (SELECT id FROM _slice_ancestor_and_self(${slice.id}))`,
            columns: ['id'],
          },
        ],
        imports: ['slices.hierarchy'],
      }),
  },
  {
    name: 'Descendant slices',
    shouldDisplay: () => true,
    run: (slice: SliceDetails, trace: Trace) =>
      addSqlTableTab(trace, {
        table: assertExists(getSqlTableDescription('slice')),
        filters: [
          {
            op: (cols) =>
              `${cols[0]} IN (SELECT id FROM _slice_descendant_and_self(${slice.id}))`,
            columns: ['id'],
          },
        ],
        imports: ['slices.hierarchy'],
      }),
  },
  {
    name: 'Average duration of slice name',
    shouldDisplay: (slice: SliceDetails) => hasName(slice),
    run: (slice: SliceDetails, trace: Trace) =>
      addQueryResultsTab(trace, {
        query: `SELECT AVG(dur) / 1e9 FROM slice WHERE name = '${slice.name!}'`,
        title: `${slice.name} average dur`,
      }),
  },
  {
    name: 'Binder txn names + monitor contention on thread',
    shouldDisplay: (slice) =>
      hasProcessName(slice) &&
      hasThreadName(slice) &&
      hasTid(slice) &&
      hasPid(slice),
    run: (slice: SliceDetails, trace: Trace) => {
      trace.engine
        .query(
          `INCLUDE PERFETTO MODULE android.binder;
           INCLUDE PERFETTO MODULE android.monitor_contention;`,
        )
        .then(() =>
          addDebugSliceTrack(
            trace,
            {
              sqlSource: `
                                WITH merged AS (
                                  SELECT s.ts, s.dur, tx.aidl_name AS name, 0 AS depth
                                  FROM android_binder_txns tx
                                  JOIN slice s
                                    ON tx.binder_txn_id = s.id
                                  JOIN thread_track
                                    ON s.track_id = thread_track.id
                                  JOIN thread
                                    USING (utid)
                                  JOIN process
                                    USING (upid)
                                  WHERE pid = ${getPidFromSlice(slice)}
                                        AND tid = ${getTidFromSlice(slice)}
                                        AND aidl_name IS NOT NULL
                                  UNION ALL
                                  SELECT
                                    s.ts,
                                    s.dur,
                                    short_blocked_method || ' -> ' || blocking_thread_name || ':' || short_blocking_method AS name,
                                    1 AS depth
                                  FROM android_binder_txns tx
                                  JOIN android_monitor_contention m
                                    ON m.binder_reply_tid = tx.server_tid AND m.binder_reply_ts = tx.server_ts
                                  JOIN slice s
                                    ON tx.binder_txn_id = s.id
                                  JOIN thread_track
                                    ON s.track_id = thread_track.id
                                  JOIN thread ON thread.utid = thread_track.utid
                                  JOIN process ON process.upid = thread.upid
                                  WHERE process.pid = ${getPidFromSlice(slice)}
                                        AND thread.tid = ${getTidFromSlice(
                                          slice,
                                        )}
                                        AND short_blocked_method IS NOT NULL
                                  ORDER BY depth
                                ) SELECT ts, dur, name FROM merged`,
            },
            `Binder names (${getProcessNameFromSlice(
              slice,
            )}:${getThreadNameFromSlice(slice)})`,
            {ts: 'ts', dur: 'dur', name: 'name'},
            [],
          ),
        );
    },
  },
];

function getSliceContextMenuItems(slice: SliceDetails) {
  return ITEMS.filter((item) => item.shouldDisplay(slice));
}

async function getAnnotationSlice(
  engine: Engine,
  id: number,
): Promise<SliceDetails | undefined> {
  const query = await engine.query(`
    SELECT
      id,
      name,
      ts,
      dur,
      track_id as trackId,
      thread_dur as threadDur,
      cat,
      ABS_TIME_STR(ts) as absTime
    FROM annotation_slice
    where id = ${id}`);

  const it = query.firstRow({
    id: NUM,
    name: STR_NULL,
    ts: LONG,
    dur: LONG,
    trackId: NUM,
    threadDur: LONG_NULL,
    cat: STR_NULL,
    absTime: STR_NULL,
  });

  return {
    id: asSliceSqlId(it.id),
    name: it.name ?? 'null',
    ts: Time.fromRaw(it.ts),
    dur: it.dur,
    depth: 0,
    trackId: it.trackId,
    threadDur: it.threadDur ?? undefined,
    category: it.cat ?? undefined,
    absTime: it.absTime ?? undefined,
  };
}

async function getSliceDetails(
  engine: Engine,
  id: number,
  table: string,
): Promise<SliceDetails | undefined> {
  if (table === 'annotation_slice') {
    return getAnnotationSlice(engine, id);
  } else {
    return getSlice(engine, asSliceSqlId(id));
  }
}

export class ThreadSliceDetailsPanel implements TrackEventDetailsPanel {
  private sliceDetails?: SliceDetails;
  private breakdownByThreadState?: BreakdownByThreadState;

  constructor(
    private readonly trace: Trace,
    private readonly tableName: string,
  ) {}

  async load({eventId}: TrackEventSelection) {
    const {trace, tableName} = this;
    const details = await getSliceDetails(trace.engine, eventId, tableName);

    if (
      details !== undefined &&
      details.thread !== undefined &&
      details.dur > 0
    ) {
      this.breakdownByThreadState = await breakDownIntervalByThreadState(
        trace.engine,
        TimeSpan.fromTimeAndDuration(details.ts, details.dur),
        details.thread.utid,
      );
    }

    this.sliceDetails = details;
  }

  render() {
    if (!exists(this.sliceDetails)) {
      return m(DetailsShell, {title: 'Slice', description: 'Loading...'});
    }
    const slice = this.sliceDetails;
    return m(
      DetailsShell,
      {
        title: 'Slice',
        description: slice.name,
        buttons: this.renderContextButton(slice),
      },
      m(
        GridLayout,
        renderDetails(this.trace, slice, this.breakdownByThreadState),
        this.renderRhs(this.trace, slice),
      ),
    );
  }

  private renderRhs(trace: Trace, slice: SliceDetails): m.Children {
    const precFlows = this.renderPrecedingFlows(slice);
    const followingFlows = this.renderFollowingFlows(slice);
    const args =
      hasArgs(slice.args) &&
      m(
        Section,
        {title: 'Arguments'},
        m(Tree, renderArguments(trace, slice.args)),
      );
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    if (precFlows ?? followingFlows ?? args) {
      return m(GridLayoutColumn, precFlows, followingFlows, args);
    } else {
      return undefined;
    }
  }

  private renderPrecedingFlows(slice: SliceDetails): m.Children {
    const flows = globals.trace.flows.connectedFlows;
    const inFlows = flows.filter(({end}) => end.sliceId === slice.id);

    if (inFlows.length > 0) {
      const isRunTask =
        slice.name === 'ThreadControllerImpl::RunTask' ||
        slice.name === 'ThreadPool_RunTask';

      return m(
        Section,
        {title: 'Preceding Flows'},
        m(BasicTable<Flow>, {
          columns: [
            {
              title: 'Slice',
              render: (flow: Flow) =>
                m(SliceRef, {
                  id: asSliceSqlId(flow.begin.sliceId),
                  name:
                    flow.begin.sliceChromeCustomName ?? flow.begin.sliceName,
                  ts: flow.begin.sliceStartTs,
                  dur: flow.begin.sliceEndTs - flow.begin.sliceStartTs,
                  sqlTrackId: flow.begin.trackId,
                }),
            },
            {
              title: 'Delay',
              render: (flow: Flow) =>
                m(DurationWidget, {
                  dur: flow.end.sliceStartTs - flow.begin.sliceEndTs,
                }),
            },
            {
              title: 'Thread',
              render: (flow: Flow) =>
                this.getThreadNameForFlow(flow.begin, !isRunTask),
            },
          ],
          data: inFlows,
        }),
      );
    } else {
      return null;
    }
  }

  private renderFollowingFlows(slice: SliceDetails): m.Children {
    const flows = globals.trace.flows.connectedFlows;
    const outFlows = flows.filter(({begin}) => begin.sliceId === slice.id);

    if (outFlows.length > 0) {
      const isPostTask =
        slice.name === 'ThreadPool_PostTask' ||
        slice.name === 'SequenceManager PostTask';

      return m(
        Section,
        {title: 'Following Flows'},
        m(BasicTable<Flow>, {
          columns: [
            {
              title: 'Slice',
              render: (flow: Flow) =>
                m(SliceRef, {
                  id: asSliceSqlId(flow.end.sliceId),
                  name: flow.end.sliceChromeCustomName ?? flow.end.sliceName,
                  ts: flow.end.sliceStartTs,
                  dur: flow.end.sliceEndTs - flow.end.sliceStartTs,
                  sqlTrackId: flow.end.trackId,
                }),
            },
            {
              title: 'Delay',
              render: (flow: Flow) =>
                m(DurationWidget, {
                  dur: flow.end.sliceStartTs - flow.begin.sliceEndTs,
                }),
            },
            {
              title: 'Thread',
              render: (flow: Flow) =>
                this.getThreadNameForFlow(flow.end, !isPostTask),
            },
          ],
          data: outFlows,
        }),
      );
    } else {
      return null;
    }
  }

  private getThreadNameForFlow(
    flow: FlowPoint,
    includeProcessName: boolean,
  ): string {
    return includeProcessName
      ? `${flow.threadName} (${flow.processName})`
      : flow.threadName;
  }

  private renderContextButton(sliceInfo: SliceDetails): m.Children {
    const contextMenuItems = getSliceContextMenuItems(sliceInfo);
    if (contextMenuItems.length > 0) {
      const trigger = m(Button, {
        compact: true,
        label: 'Contextual Options',
        rightIcon: Icons.ContextMenu,
      });
      return m(
        PopupMenu2,
        {trigger},
        contextMenuItems.map(({name, run}) =>
          m(MenuItem, {label: name, onclick: () => run(sliceInfo, this.trace)}),
        ),
      );
    } else {
      return undefined;
    }
  }
}
