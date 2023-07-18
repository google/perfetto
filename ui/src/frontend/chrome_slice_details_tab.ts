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

import {BigintMath} from '../base/bigint_math';
import {sqliteString} from '../base/string_utils';
import {exists} from '../base/utils';
import {Actions} from '../common/actions';
import {EngineProxy} from '../common/engine';
import {runQuery} from '../common/queries';
import {LONG, LONG_NULL, NUM, STR_NULL} from '../common/query_result';
import {Duration, duration, Time, time} from '../common/time';
import {ArgNode, convertArgsToTree, Key} from '../controller/args_parser';

import {Anchor} from './anchor';
import {
  addTab,
  BottomTab,
  bottomTabRegistry,
  NewBottomTabArgs,
} from './bottom_tab';
import {FlowPoint, globals} from './globals';
import {PanelSize} from './panel';
import {runQueryInNewTab} from './query_result_tab';
import {Icons} from './semantic_icons';
import {Arg} from './sql/args';
import {getSlice, SliceDetails, SliceRef} from './sql/slice';
import {SqlTableTab} from './sql_table/tab';
import {SqlTables} from './sql_table/well_known_tables';
import {asSliceSqlId} from './sql_types';
import {getProcessName, getThreadName} from './thread_and_process_info';
import {Button} from './widgets/button';
import {DetailsShell} from './widgets/details_shell';
import {DurationWidget} from './widgets/duration';
import {GridLayout, GridLayoutColumn} from './widgets/grid_layout';
import {MenuItem, PopupMenu2} from './widgets/menu';
import {Section} from './widgets/section';
import {SqlRef} from './widgets/sql_ref';
import {Timestamp} from './widgets/timestamp';
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

function hasName(slice: SliceDetails): boolean {
  return slice.name !== undefined;
}

function hasId(slice: SliceDetails): boolean {
  return slice.id !== undefined;
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

const ITEMS: ContextMenuItem[] = [
  {
    name: 'Average duration',
    shouldDisplay: (slice: SliceDetails) => hasName(slice),
    run: (slice: SliceDetails) => runQueryInNewTab(
        `SELECT AVG(dur) / 1e9 FROM slice WHERE name = '${slice.name!}'`,
        `${slice.name} average dur`,
        ),
  },
  {
    name: 'Binder by TXN',
    shouldDisplay: () => true,
    run: () => runQueryInNewTab(
        `SELECT IMPORT('android.binder');

         SELECT *
         FROM android_sync_binder_metrics_by_txn
         ORDER BY client_dur DESC`,
        'Binder by TXN',
        ),
  },
  {
    name: 'Binder call names',
    shouldDisplay: (slice) =>
        hasProcessName(slice) && hasTid(slice) && hasPid(slice),
    run: (slice: SliceDetails) => {
      const engine = getEngine();
      if (engine === undefined) return;
      runQuery(`SELECT IMPORT('android.binder');`, engine)
          .then(
              () => runQueryInNewTab(
                  `
                SELECT s.ts, s.dur, tx.aidl_name AS name, s.id
                FROM android_sync_binder_metrics_by_txn tx
                  JOIN slice s ON tx.binder_txn_id = s.id
                  JOIN thread_track ON s.track_id = thread_track.id
                  JOIN thread USING (utid)
                  JOIN process USING (upid)
                WHERE aidl_name IS NOT NULL
                  AND pid = ${getPidFromSlice(slice)}
                  AND tid = ${getTidFromSlice(slice)}`,
                  `Binder names (${getProcessNameFromSlice(slice)}:${
                      getTidFromSlice(slice)})`,
                  ));
    },
  },
  {
    name: 'Lock graph',
    shouldDisplay: (slice: SliceDetails) => hasId(slice),
    run: (slice: SliceDetails) => runQueryInNewTab(
        `SELECT IMPORT('android.monitor_contention');
         DROP TABLE IF EXISTS FAST;
         CREATE TABLE FAST
         AS
         WITH slice_process AS (
         SELECT process.name, process.upid FROM slice
         JOIN thread_track ON thread_track.id = slice.track_id
         JOIN thread USING(utid)
         JOIN process USING(upid)
         WHERE slice.id = ${slice.id}
         )
         SELECT *,
         IIF(blocked_thread_name LIKE 'binder:%', 'binder', blocked_thread_name)
          AS blocked_thread_name_norm,
         IIF(blocking_thread_name LIKE 'binder:%', 'binder', blocking_thread_name)
          AS blocking_thread_name_norm
         FROM android_monitor_contention_chain, slice_process
         WHERE android_monitor_contention_chain.upid = slice_process.upid;

         WITH
         R AS (
         SELECT
           id,
           dur,
           CAT_STACKS(blocked_thread_name_norm || ':' || short_blocked_method,
             blocking_thread_name_norm || ':' || short_blocking_method) AS stack
         FROM FAST
         WHERE parent_id IS NULL
         UNION ALL
         SELECT
         c.id,
         c.dur AS dur,
         CAT_STACKS(stack, blocking_thread_name_norm || ':' || short_blocking_method) AS stack
         FROM FAST c, R AS p
         WHERE p.id = c.parent_id
         )
         SELECT TITLE.process_name, EXPERIMENTAL_PROFILE(stack, 'duration', 'ns', dur) AS pprof
         FROM R, (SELECT process_name FROM FAST LIMIT 1) TITLE;`,
        'Lock graph',
        ),
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

function renderArgKey(key: string, value?: Arg): m.Children {
  if (value === undefined) {
    return key;
  } else {
    const {key: fullKey, displayValue} = value;
    return m(
        PopupMenu2,
        {trigger: m(Anchor, {icon: Icons.ContextMenu}, key)},
        m(MenuItem, {
          label: 'Copy full key',
          icon: 'content_copy',
          onclick: () => navigator.clipboard.writeText(fullKey),
        }),
        value && m(MenuItem, {
          label: 'Find slices with same arg value',
          icon: 'search',
          onclick: () => {
            addTab({
              kind: SqlTableTab.kind,
              config: {
                table: SqlTables.slice,
                filters: [{
                  type: 'arg_filter',
                  argSetIdColumn: 'arg_set_id',
                  argName: fullKey,
                  op: `= ${sqliteString(displayValue)}`,
                }],
              },
            });
          },
        }),
        value && m(MenuItem, {
          label: 'Visualise argument values',
          icon: 'query_stats',
          onclick: () => {
            globals.dispatch(Actions.addVisualisedArg({argName: fullKey}));
          },
        }),
    );
  }
}

function isWebLink(value: unknown): value is string {
  return typeof value === 'string' &&
      (value.startsWith('http://') || value.startsWith('https://'));
}

// Try to render arg value as a special value, otherwise just render the text.
function renderArgValue({value}: Arg): m.Children {
  if (isWebLink(value)) {
    return renderWebLink(value);
  } else {
    return `${value}`;
  }
}

function renderWebLink(url: string): m.Children {
  return m(Anchor, {href: url, target: '_blank', icon: 'open_in_new'}, url);
}

function renderSummary(children: ArgNode<Arg>[]): m.Children {
  const summary = children.slice(0, 2).map(({key}) => key).join(', ');
  const remaining = children.length - 2;
  if (remaining > 0) {
    return `{${summary}, ... (${remaining} more items)}`;
  } else {
    return `{${summary}}`;
  }
}

// Format any number of keys into a composite key with standardized formatting.
function stringifyKey(...key: Key[]): string {
  return key
      .map((element, index) => {
        if (typeof element === 'number') {
          return `[${element}]`;
        } else {
          return (index === 0 ? '' : '.') + element;
        }
      })
      .join('');
}

function renderArgTreeNodes(
    engine: EngineProxy, args: ArgNode<Arg>[]): m.Children {
  return args.map((arg) => {
    const {key, value, children} = arg;
    if (children && children.length === 1) {
      // If we only have one child, collapse into self and combine keys
      const child = children[0];
      const compositeArg = {
        ...child,
        key: stringifyKey(key, child.key),
      };
      return renderArgTreeNodes(engine, [compositeArg]);
    } else {
      return m(
          TreeNode,
          {
            left: renderArgKey(stringifyKey(key), value),
            right: exists(value) && renderArgValue(value),
            summary: children && renderSummary(children),
          },
          children && renderArgTreeNodes(engine, children),
      );
    }
  });
}

function computeDuration(ts: time, dur: duration): m.Children {
  if (dur === -1n) {
    const minDuration = globals.state.traceTime.end - ts;
    return `${Duration.format(minDuration)} (Did not end)`;
  } else {
    return m(DurationWidget, {dur});
  }
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
              this.renderDetails(slice),
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
    const args = this.renderArguments(engine, slice);
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

  private renderDetails(slice: SliceDetails) {
    return m(
        Section,
        {title: 'Details'},
        m(
            Tree,
            m(TreeNode, {
              left: 'Name',
              right: m(
                  PopupMenu2,
                  {
                    trigger: m(Anchor, slice.name),
                  },
                  m(MenuItem, {
                    label: 'Slices with the same name',
                    onclick: () => {
                      addTab({
                        kind: SqlTableTab.kind,
                        config: {
                          table: SqlTables.slice,
                          displayName: 'slice',
                          filters: [`name = ${sqliteString(slice.name)}`],
                        },
                      });
                    },
                  }),
                  ),
            }),
            m(TreeNode, {
              left: 'Category',
              right: !slice.category || slice.category === '[NULL]' ?
                  'N/A' :
                  slice.category,
            }),
            m(TreeNode, {
              left: 'Start time',
              right: m(Timestamp, {ts: slice.ts}),
            }),
            exists(slice.absTime) &&
                m(TreeNode, {left: 'Absolute Time', right: slice.absTime}),
            m(TreeNode, {
              left: 'Duration',
              right: computeDuration(slice.ts, slice.dur),
            }),
            this.renderThreadDuration(slice),
            slice.thread && m(TreeNode, {
              left: 'Thread',
              right: getThreadName(slice.thread),
            }),
            slice.process && m(TreeNode, {
              left: 'Process',
              right: getProcessName(slice.process),
            }),
            slice.process && exists(slice.process.uid) && m(TreeNode, {
              left: 'User ID',
              right: slice.process.uid,
            }),
            slice.process && slice.process.packageName && m(TreeNode, {
              left: 'Package name',
              right: slice.process.packageName,
            }),
            slice.process && exists(slice.process.versionCode) && m(TreeNode, {
              left: 'Version code',
              right: slice.process.versionCode,
            }),
            m(TreeNode, {
              left: 'SQL ID',
              right: m(SqlRef, {table: 'slice', id: slice.id}),
            }),
            ));
  }

  private renderThreadDuration(sliceInfo: SliceDetails) {
    if (exists(sliceInfo.threadTs) && exists(sliceInfo.threadDur)) {
      // If we have valid thread duration, also display a percentage of
      // |threadDur| compared to |dur|.
      const ratio = BigintMath.ratio(sliceInfo.threadDur, sliceInfo.dur);
      const threadDurFractionSuffix =
          sliceInfo.threadDur === -1n ? '' : ` (${(ratio * 100).toFixed(2)}%)`;
      return m(TreeNode, {
        left: 'Thread duration',
        right: [
          computeDuration(sliceInfo.threadTs, sliceInfo.threadDur),
          threadDurFractionSuffix,
        ],
      });
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

  private renderArguments(engine: EngineProxy, slice: SliceDetails):
      m.Children {
    if (slice.args && slice.args.length > 0) {
      const tree = convertArgsToTree(slice.args);
      return m(
          Section,
          {title: 'Arguments'},
          m(Tree, renderArgTreeNodes(engine, tree)));
    } else {
      return undefined;
    }
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
