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
import {Icons} from '../../base/semantic_icons';
import {TimeSpan} from '../../base/time';
import {exists} from '../../base/utils';
import {Engine} from '../../trace_processor/engine';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {MenuItem, PopupMenu} from '../../widgets/menu';
import {Section} from '../../widgets/section';
import {Tree} from '../../widgets/tree';
import {FlowPoint} from '../../core/flow_types';
import {hasArgs} from './args';
import {renderDetails} from './slice_details';
import {getSlice, SliceDetails} from '../sql_utils/slice';
import {
  BreakdownByThreadState,
  breakDownIntervalByThreadState,
} from './thread_state';
import {asSliceSqlId} from '../sql_utils/core_types';
import {DurationWidget} from '../widgets/duration';
import {SliceRef} from '../widgets/slice';
import {Grid, GridCell, GridHeaderCell} from '../../widgets/grid';
import {assertIsInstance} from '../../base/logging';
import {Trace} from '../../public/trace';
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {TrackEventSelection} from '../../public/selection';
import {extensions} from '../extensions';
import {TraceImpl} from '../../core/trace_impl';
import {renderSliceArguments} from './slice_args';
import {SLICE_TABLE} from '../widgets/sql/table_definitions';

interface ContextMenuItem {
  name: string;
  shouldDisplay(slice: SliceDetails): boolean;
  run(slice: SliceDetails, trace: Trace): void;
}

function getTidFromSlice(slice: SliceDetails): bigint | undefined {
  return slice.thread?.tid;
}

function getPidFromSlice(slice: SliceDetails): bigint | undefined {
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
      extensions.addLegacySqlTableTab(trace, {
        table: SLICE_TABLE,
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
      extensions.addLegacySqlTableTab(trace, {
        table: SLICE_TABLE,
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
      extensions.addQueryResultsTab(trace, {
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
          extensions.addDebugSliceTrack({
            trace,
            data: {
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
            title: `Binder names (${getProcessNameFromSlice(
              slice,
            )}:${getThreadNameFromSlice(slice)})`,
          }),
        );
    },
  },
];

function getSliceContextMenuItems(slice: SliceDetails) {
  return ITEMS.filter((item) => item.shouldDisplay(slice));
}

async function getSliceDetails(
  engine: Engine,
  id: number,
): Promise<SliceDetails | undefined> {
  return getSlice(engine, asSliceSqlId(id));
}

// Interface for additional sections that can be composed
// with ThreadSliceDetailsPanel
export interface TrackEventDetailsPanelSection {
  load(selection: TrackEventSelection): Promise<void>;
  render(): m.Children;
}

export interface ThreadSliceDetailsPanelAttrs {
  // Optional additional sections to render in the left column
  leftSections?: TrackEventDetailsPanelSection[];
  // Optional additional sections to render in the right column
  rightSections?: TrackEventDetailsPanelSection[];
}

export class ThreadSliceDetailsPanel implements TrackEventDetailsPanel {
  private sliceDetails?: SliceDetails;
  private breakdownByThreadState?: BreakdownByThreadState;
  private readonly trace: TraceImpl;
  private readonly attrs: ThreadSliceDetailsPanelAttrs;

  constructor(trace: Trace, attrs?: ThreadSliceDetailsPanelAttrs) {
    // Rationale for the assertIsInstance: ThreadSliceDetailsPanel requires a
    // TraceImpl (because of flows) but here we must take a Trace interface,
    // because this track is exposed to plugins (which see only Trace).
    this.trace = assertIsInstance(trace, TraceImpl);
    this.attrs = attrs ?? {};
  }

  async load(selection: TrackEventSelection) {
    const {trace} = this;
    const {eventId} = selection;
    const details = await getSliceDetails(trace.engine, eventId);

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

    // Load additional sections
    const sectionsToLoad = [
      ...(this.attrs.leftSections ?? []),
      ...(this.attrs.rightSections ?? []),
    ];
    if (sectionsToLoad.length > 0) {
      await Promise.all(
        sectionsToLoad.map((section) => section.load(selection)),
      );
    }
  }

  render() {
    if (!exists(this.sliceDetails)) {
      return m(DetailsShell, {title: 'Slice', description: 'Loading...'});
    }
    const slice = this.sliceDetails;

    // Render additional left and right sections
    const additionalLeft = this.attrs.leftSections?.map((section) =>
      section.render(),
    );
    const additionalRight = this.attrs.rightSections?.map((section) =>
      section.render(),
    );

    return m(
      DetailsShell,
      {
        title: 'Slice',
        description: slice.name,
        buttons: this.renderContextButton(slice),
      },
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          renderDetails(this.trace, slice, this.breakdownByThreadState),
          additionalLeft,
        ),
        this.renderRhs(this.trace, slice, additionalRight),
      ),
    );
  }

  private renderRhs(
    trace: Trace,
    slice: SliceDetails,
    additionalSections?: m.Children,
  ): m.Children {
    const precFlows = this.renderPrecedingFlows(slice);
    const followingFlows = this.renderFollowingFlows(slice);
    const args =
      hasArgs(slice.args) &&
      m(
        Section,
        {title: 'Arguments'},
        m(Tree, renderSliceArguments(trace, slice.args)),
      );
    if (
      precFlows !== undefined ||
      followingFlows !== undefined ||
      args !== undefined ||
      additionalSections !== undefined
    ) {
      return m(
        GridLayoutColumn,
        precFlows,
        followingFlows,
        args,
        additionalSections,
      );
    } else {
      return undefined;
    }
  }

  private renderPrecedingFlows(slice: SliceDetails): m.Children {
    const flows = this.trace.flows.connectedFlows;
    const inFlows = flows.filter(({end}) => end.sliceId === slice.id);

    if (inFlows.length > 0) {
      const isRunTask =
        slice.name === 'ThreadControllerImpl::RunTask' ||
        slice.name === 'ThreadPool_RunTask';

      return m(
        Section,
        {title: 'Preceding Flows'},
        m(Grid, {
          columns: [
            {key: 'sliceName', header: m(GridHeaderCell, 'Slice')},
            {key: 'delay', header: m(GridHeaderCell, 'Delay')},
            {key: 'thread', header: m(GridHeaderCell, 'Thread')},
          ],
          rowData: inFlows.map((flow) => [
            m(
              GridCell,
              m(SliceRef, {
                trace: this.trace,
                id: asSliceSqlId(flow.begin.sliceId),
                name: flow.begin.sliceChromeCustomName ?? flow.begin.sliceName,
              }),
            ),
            m(
              GridCell,
              m(DurationWidget, {
                trace: this.trace,
                dur: flow.end.sliceStartTs - flow.begin.sliceEndTs,
              }),
            ),
            m(GridCell, this.getThreadNameForFlow(flow.begin, !isRunTask)),
          ]),
        }),
      );
    } else {
      return null;
    }
  }

  private renderFollowingFlows(slice: SliceDetails): m.Children {
    const flows = this.trace.flows.connectedFlows;
    const outFlows = flows.filter(({begin}) => begin.sliceId === slice.id);

    if (outFlows.length > 0) {
      const isPostTask =
        slice.name === 'ThreadPool_PostTask' ||
        slice.name === 'SequenceManager PostTask';

      return m(
        Section,
        {title: 'Following Flows'},
        m(Grid, {
          columns: [
            {key: 'slice', header: m(GridHeaderCell, 'Slice')},
            {key: 'delay', header: m(GridHeaderCell, 'Delay')},
            {key: 'thread', header: m(GridHeaderCell, 'Thread')},
          ],
          rowData: outFlows.map((flow) => [
            m(
              GridCell,
              m(SliceRef, {
                trace: this.trace,
                id: asSliceSqlId(flow.end.sliceId),
                name: flow.end.sliceChromeCustomName ?? flow.end.sliceName,
              }),
            ),
            m(
              GridCell,
              m(DurationWidget, {
                trace: this.trace,
                dur: flow.end.sliceStartTs - flow.begin.sliceEndTs,
              }),
            ),
            m(GridCell, this.getThreadNameForFlow(flow.end, !isPostTask)),
          ]),
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
        PopupMenu,
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
