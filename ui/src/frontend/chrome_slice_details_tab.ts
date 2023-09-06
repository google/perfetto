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

import {duration, Time} from '../base/time';
import {exists} from '../base/utils';
import {EngineProxy} from '../common/engine';
import {runQuery} from '../common/queries';
import {LONG, LONG_NULL, NUM, STR_NULL} from '../common/query_result';
import {addDebugTrack} from '../tracks/debug/slice_track';

import {
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from './bottom_tab';
import {FlowPoint, globals} from './globals';
import {PanelSize} from './panel';
import {runQueryInNewTab} from './query_result_tab';
import {Icons} from './semantic_icons';
import {renderArguments} from './slice_args';
import {renderDetails} from './slice_details';
import {getSlice, SliceDetails, SliceRef} from './sql/slice';
import {asSliceSqlId} from './sql_types';
import {Button} from './widgets/button';
import {DetailsShell} from './widgets/details_shell';
import {DurationWidget} from './widgets/duration';
import {GridLayout, GridLayoutColumn} from './widgets/grid_layout';
import {MenuItem, PopupMenu2} from './widgets/menu';
import {Section} from './widgets/section';
import {Tree, TreeNode} from './widgets/tree';

interface ContextMenuItem {
  name: string;
  shouldDisplay(slice: SliceDetails): boolean;
  run(slice: SliceDetails): void;
}

function getTidFromSlice(slice: SliceDetails): number|undefined {
  return slice.thread?.tid;
}

function getPidFromSlice(slice: SliceDetails): number|undefined {
  return slice.process?.pid;
}

function getProcessNameFromSlice(slice: SliceDetails): string|undefined {
  return slice.process?.name;
}

function getThreadNameFromSlice(slice: SliceDetails): string|undefined {
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
    name: 'Average duration of slice name',
    shouldDisplay: (slice: SliceDetails) => hasName(slice),
    run: (slice: SliceDetails) => runQueryInNewTab(
        `SELECT AVG(dur) / 1e9 FROM slice WHERE name = '${slice.name!}'`,
        `${slice.name} average dur`,
        ),
  },
  {
    name: 'Binder call names on thread',
    shouldDisplay: (slice) => hasProcessName(slice) && hasThreadName(slice) &&
        hasTid(slice) && hasPid(slice),
    run: (slice: SliceDetails) => {
      const engine = getEngine();
      if (engine === undefined) return;
      runQuery(`SELECT IMPORT('android.binder');`, engine)
          .then(
              () => addDebugTrack(
                  engine,
                  {
                    sqlSource: `
                            SELECT s.ts, s.dur, tx.aidl_name AS name
                            FROM android_sync_binder_metrics_by_txn tx
                              JOIN slice s ON tx.binder_txn_id = s.id
                              JOIN thread_track ON s.track_id = thread_track.id
                              JOIN thread USING (utid)
                              JOIN process USING (upid)
                            WHERE aidl_name IS NOT NULL
                              AND pid = ${getPidFromSlice(slice)}
                              AND tid = ${getTidFromSlice(slice)}`,
                    columns: ['ts', 'dur', 'name'],
                  },
                  `Binder names (${getProcessNameFromSlice(slice)}:${
                      getThreadNameFromSlice(slice)})`,
                  {ts: 'ts', dur: 'dur', name: 'name'},
                  [],
                  ));
    },
  },
];

function getSliceContextMenuItems(slice: SliceDetails) {
  return ITEMS.filter((item) => item.shouldDisplay(slice));
}

function getEngine(): EngineProxy|undefined {
  const engineId = globals.getCurrentEngine()?.id;
  if (engineId === undefined) {
    return undefined;
  }
  const engine = globals.engines.get(engineId)?.getProxy('SlicePanel');
  return engine;
}

async function getAnnotationSlice(
    engine: EngineProxy, id: number): Promise<SliceDetails|undefined> {
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
    sqlTrackId: it.trackId,
    threadDur: it.threadDur ?? undefined,
    category: it.cat ?? undefined,
    absTime: it.absTime ?? undefined,
  };
}

async function getSliceDetails(engine: EngineProxy, id: number, table: string):
    Promise<SliceDetails|undefined> {
  if (table === 'annotation') {
    return getAnnotationSlice(engine, id);
  } else {
    return getSlice(engine, asSliceSqlId(id));
  }
}

interface ChromeSliceDetailsTabConfig {
  id: number;
  table: string;
}

export class ChromeSliceDetailsTab extends
    BottomTab<ChromeSliceDetailsTabConfig> {
  static readonly kind = 'dev.perfetto.ChromeSliceDetailsTab';

  private sliceDetails?: SliceDetails;

  static create(args: NewBottomTabArgs): ChromeSliceDetailsTab {
    return new ChromeSliceDetailsTab(args);
  }

  constructor(args: NewBottomTabArgs) {
    super(args);

    // Start loading the slice details
    const {id, table} = this.config;
    getSliceDetails(this.engine, id, table)
        .then((sliceDetails) => this.sliceDetails = sliceDetails);
  }

  renderTabCanvas(_ctx: CanvasRenderingContext2D, _size: PanelSize): void {
    // No-op
  }

  getTitle(): string {
    return `Current Selection`;
  }

  viewTab() {
    if (exists(this.sliceDetails)) {
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
              renderDetails(slice),
              this.renderRhs(this.engine, slice),
              ),
      );
    } else {
      return m(DetailsShell, {title: 'Slice', description: 'Loading...'});
    }
  }

  isLoading() {
    return !exists(this.sliceDetails);
  }

  private renderRhs(engine: EngineProxy, slice: SliceDetails): m.Children {
    const precFlows = this.renderPrecedingFlows(slice);
    const followingFlows = this.renderFollowingFlows(slice);
    const args = renderArguments(engine, slice);
    if (precFlows ?? followingFlows ?? args) {
      return m(
          GridLayoutColumn,
          precFlows,
          followingFlows,
          args,
      );
    } else {
      return undefined;
    }
  }

  private renderPrecedingFlows(slice: SliceDetails): m.Children {
    const flows = globals.connectedFlows;
    const inFlows = flows.filter(({end}) => end.sliceId === slice.id);

    if (inFlows.length > 0) {
      const isRunTask = slice.name === 'ThreadControllerImpl::RunTask' ||
          slice.name === 'ThreadPool_RunTask';

      return m(
          Section,
          {title: 'Preceding Flows'},
          m(
              Tree,
              inFlows.map(
                  ({begin, dur}) => this.renderFlow(begin, dur, !isRunTask)),
              ));
    } else {
      return null;
    }
  }

  private renderFollowingFlows(slice: SliceDetails): m.Children {
    const flows = globals.connectedFlows;
    const outFlows = flows.filter(({begin}) => begin.sliceId === slice.id);

    if (outFlows.length > 0) {
      const isPostTask = slice.name === 'ThreadPool_PostTask' ||
          slice.name === 'SequenceManager PostTask';

      return m(
          Section,
          {title: 'Following Flows'},
          m(
              Tree,
              outFlows.map(
                  ({end, dur}) => this.renderFlow(end, dur, !isPostTask)),
              ));
    } else {
      return null;
    }
  }

  private renderFlow(
      flow: FlowPoint, dur: duration, includeProcessName: boolean): m.Children {
    const description = flow.sliceChromeCustomName === undefined ?
        flow.sliceName :
        flow.sliceChromeCustomName;
    const threadName = includeProcessName ?
        `${flow.threadName} (${flow.processName})` :
        flow.threadName;

    return m(
        TreeNode,
        {left: 'Flow'},
        m(TreeNode, {
          left: 'Slice',
          right: m(SliceRef, {
            id: asSliceSqlId(flow.sliceId),
            name: description,
            ts: flow.sliceStartTs,
            dur: flow.sliceEndTs - flow.sliceStartTs,
            sqlTrackId: flow.trackId,
          }),
        }),
        m(TreeNode, {left: 'Delay', right: m(DurationWidget, {dur})}),
        m(TreeNode, {left: 'Thread', right: threadName}),
    );
  }

  private renderContextButton(sliceInfo: SliceDetails): m.Children {
    const contextMenuItems = getSliceContextMenuItems(sliceInfo);
    if (contextMenuItems.length > 0) {
      const trigger = m(Button, {
        minimal: true,
        compact: true,
        label: 'Contextual Options',
        rightIcon: Icons.ContextMenu,
      });
      return m(
          PopupMenu2,
          {trigger},
          contextMenuItems.map(
              ({name, run}) =>
                  m(MenuItem, {label: name, onclick: () => run(sliceInfo)})),
      );
    } else {
      return undefined;
    }
  }
}

bottomTabRegistry.register(ChromeSliceDetailsTab);
