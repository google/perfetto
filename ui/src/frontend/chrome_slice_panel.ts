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

import {sqliteString} from '../base/string_utils';
import {Actions} from '../common/actions';
import {ArgValue} from '../common/arg_types';
import {EngineProxy} from '../common/engine';
import {runQuery} from '../common/queries';
import {
  TPDuration,
  tpDurationToSeconds,
  TPTime,
  tpTimeToCode,
} from '../common/time';
import {Argument, convertArgsToTree, Key} from '../controller/args_parser';

import {Anchor} from './anchor';
import {FlowPoint, globals, SliceDetails} from './globals';
import {runQueryInNewTab} from './query_result_tab';
import {verticalScrollToTrack} from './scroll_helper';
import {Icons} from './semantic_icons';
import {asTPTimestamp} from './sql_types';
import {Button} from './widgets/button';
import {DetailsShell} from './widgets/details_shell';
import {Column, GridLayout} from './widgets/grid_layout';
import {MenuItem, PopupMenu2} from './widgets/menu';
import {Section} from './widgets/section';
import {SqlRef} from './widgets/sql_ref';
import {Timestamp} from './widgets/timestamp';
import {Tree, TreeNode} from './widgets/tree';
import {exists} from './widgets/utils';

interface ContextMenuItem {
  name: string;
  shouldDisplay(slice: SliceDetails): boolean;
  getAction(slice: SliceDetails): void;
}

const ITEMS: ContextMenuItem[] = [
  {
    name: 'Average duration',
    shouldDisplay: (slice: SliceDetails) => slice.name !== undefined,
    getAction: (slice: SliceDetails) => runQueryInNewTab(
        `SELECT AVG(dur) / 1e9 FROM slice WHERE name = '${slice.name!}'`,
        `${slice.name} average dur`,
        ),
  },
  {
    name: 'Binder by TXN',
    shouldDisplay: () => true,
    getAction: () => runQueryInNewTab(
        `SELECT IMPORT('android.binder');

         SELECT *
         FROM android_sync_binder_metrics_by_txn
         ORDER BY client_dur DESC`,
        'Binder by TXN',
        ),
  },
  {
    name: 'Binder call names',
    shouldDisplay: () => true,
    getAction: (slice: SliceDetails) => {
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
                  AND pid = ${slice.pid}
                  AND tid = ${slice.tid}`,
                  `Binder names (${slice.processName}:${slice.tid})`,
                  ));
    },
  },
  {
    name: 'Lock graph',
    shouldDisplay: (slice: SliceDetails) => slice.id !== undefined,
    getAction: (slice: SliceDetails) => runQueryInNewTab(
        `SELECT IMPORT('android.monitor_contention');
         DROP TABLE IF EXISTS FAST;
         CREATE TABLE FAST
         AS
         WITH slice_process AS (
         SELECT process.name, process.upid FROM slice
         JOIN thread_track ON thread_track.id = slice.track_id
         JOIN thread USING(utid)
         JOIN process USING(upid)
         WHERE slice.id = ${slice.id!}
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

function getArgValueRaw(value: ArgValue): string {
  if (typeof value === 'object') {
    return value.rawValue;
  } else {
    return value;
  }
}

// Renders a key with a button to get dropdown things
function renderArgKey(
    key: string, fullKey?: string, value?: ArgValue): m.Children {
  if (value === undefined || fullKey === undefined) {
    return key;
  } else {
    return m(
        PopupMenu2,
        {trigger: m(Anchor, {icon: Icons.ContextMenu}, key)},
        fullKey && m(MenuItem, {
          label: 'Copy full key',
          icon: 'content_copy',
          onclick: () => {
            navigator.clipboard.writeText(fullKey);
          },
        }),
        value && fullKey && m(MenuItem, {
          label: 'Find slices with same arg value',
          icon: 'search',
          onclick: () => {
            runQueryInNewTab(
                `
              select slice.*
              from slice
              join args using (arg_set_id)
              where key=${sqliteString(fullKey)} and display_value=${
                    sqliteString(getArgValueRaw(value))}
          `,
                `Arg: ${sqliteString(fullKey)}=${
                    sqliteString(getArgValueRaw(value))}`);
          },
        }),
        value && fullKey && m(MenuItem, {
          label: 'Visualise argument values',
          icon: 'query_stats',
          onclick: () => {
            globals.dispatch(Actions.addVisualisedArg({argName: fullKey}));
          },
        }),
    );
  }
}

// Try to render arg value as a special value, otherwise just render the text.
function renderArgValue(value: ArgValue): m.Children {
  if (typeof value === 'object' && 'kind' in value) {
    const {kind} = value;
    if (kind === 'SLICE') {
      // Value looks like a slice link.
      const {sliceId, trackId} = value;
      return renderSliceLink(sliceId, trackId, `slice[${sliceId}]`);
    } else {
      const x: never = kind;
      throw new Error(`No support for args of kind '${x}'`);
    }
  } else if (typeof value === 'string') {
    if (value.startsWith('http://') || value.startsWith('https://')) {
      // Value looks like a web link.
      return m(
          Anchor, {href: value, target: '_blank', icon: 'open_in_new'}, value);
    } else {
      // Value is nothing special.
      return value;
    }
  } else {
    const x: never = value;
    throw new Error(`Unable to process '${x}' as an arg value`);
  }
}

function renderSliceLink(id: number, trackId: string, name: string) {
  return m(
      Anchor,
      {
        icon: 'call_made',
        onclick: () => {
          globals.makeSelection(
              Actions.selectChromeSlice({id, trackId, table: 'slice'}));
          // Ideally we want to have a callback to
          // findCurrentSelection after this selection has been
          // made. Here we do not have the info for horizontally
          // scrolling to ts.
          verticalScrollToTrack(trackId, true);
        },
      },
      name);
}

function renderSummary(children: Argument<ArgValue>[]): m.Children {
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

function renderArgTreeNodes(args: Argument<ArgValue>[]): m.Children {
  return args.map((arg) => {
    const {key, path, value, children} = arg;
    if (children && children.length === 1) {
      // If we only have one child, collapse into self and combine keys
      const child = children[0];
      const compositeArg = {
        ...child,
        key: stringifyKey(key, child.key),
      };
      return renderArgTreeNodes([compositeArg]);
    } else {
      return m(
          TreeNode,
          {
            left: renderArgKey(stringifyKey(key), path, value),
            right: exists(value) && renderArgValue(value),
            summary: children && renderSummary(children),
          },
          children && renderArgTreeNodes(children),
      );
    }
  });
}

interface Sliceish extends SliceDetails {
  ts: TPTime;
  dur: TPDuration;
  name: string;
}

function isSliceish(slice: SliceDetails): slice is Sliceish {
  return exists(slice.ts) && exists(slice.dur) && exists(slice.name);
}

function getDisplayName(name: string|undefined, id: number|undefined): string|
    undefined {
  if (name === undefined) {
    return id === undefined ? undefined : `${id}`;
  } else {
    return id === undefined ? name : `${name} ${id}`;
  }
}

function computeDuration(ts: TPTime, dur: TPDuration): string {
  return dur === -1n ? `${globals.state.traceTime.end - ts} (Did not end)` :
                       tpTimeToCode(dur);
}

export class ChromeSliceDetailsPanel implements m.ClassComponent {
  view() {
    const slice = globals.sliceDetails;
    if (isSliceish(slice)) {
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
              this.renderRhs(slice),
              ),
      );
    } else {
      return m(DetailsShell, {title: 'Slice', description: 'Loading...'});
    }
  }

  private renderRhs(slice: Sliceish): m.Children {
    const precFlows = this.renderPrecedingFlows(slice);
    const followingFlows = this.renderFollowingFlows(slice);
    const args = this.renderArguments(slice);
    if (precFlows ?? followingFlows ?? args) {
      return m(
          Column,
          precFlows,
          followingFlows,
          args,
      );
    } else {
      return undefined;
    }
  }

  private renderDetails(slice: Sliceish) {
    return m(
        Section,
        {title: 'Details'},
        m(Tree,
          m(TreeNode, {left: 'Name', right: slice.name}),
          m(TreeNode, {
            left: 'Category',
            right: !slice.category || slice.category === '[NULL]' ?
                'N/A' :
                slice.category,
          }),
          m(TreeNode, {
            left: 'Start time',
            right: m(Timestamp, {ts: asTPTimestamp(slice.ts)}),
          }),
          exists(slice.absTime) &&
              m(TreeNode, {left: 'Absolute Time', right: slice.absTime}),
          m(TreeNode, {
            left: 'Duration',
            right: computeDuration(slice.ts, slice.dur),
          }),
          this.renderThreadDuration(slice),
          Array.from(this.getProcessThreadDetails(slice))
              .map(
                  ([key, value]) =>
                      exists(value) && m(TreeNode, {left: key, right: value})),
          m(TreeNode, {
            left: 'SQL ID',
            right: m(SqlRef, {table: 'slice', id: slice.id}),
          }),
          slice.description &&
              Array.from(slice.description)
                  .map(
                      ([key, value]) => m(TreeNode, {left: key, right: value}),
                      )));
  }

  private getProcessThreadDetails(sliceInfo: SliceDetails) {
    return new Map<string, string|undefined>([
      ['Thread', getDisplayName(sliceInfo.threadName, sliceInfo.tid)],
      ['Process', getDisplayName(sliceInfo.processName, sliceInfo.pid)],
      ['User ID', sliceInfo.uid ? String(sliceInfo.uid) : undefined],
      ['Package name', sliceInfo.packageName],
      [
        'Version code',
        sliceInfo.versionCode ? String(sliceInfo.versionCode) : undefined,
      ],
    ]);
  }

  private renderThreadDuration(sliceInfo: Sliceish) {
    if (exists(sliceInfo.threadTs) && exists(sliceInfo.threadDur)) {
      // If we have valid thread duration, also display a percentage of
      // |threadDur| compared to |dur|.
      const ratio = tpDurationToSeconds(sliceInfo.threadDur) /
          tpDurationToSeconds(sliceInfo.dur);
      const threadDurFractionSuffix =
          sliceInfo.threadDur === -1n ? '' : ` (${(ratio * 100).toFixed(2)}%)`;
      return m(TreeNode, {
        left: 'Thread duration',
        right: computeDuration(sliceInfo.threadTs, sliceInfo.threadDur) +
            threadDurFractionSuffix,
      });
    } else {
      return undefined;
    }
  }

  private renderPrecedingFlows(slice: Sliceish): m.Children {
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

  private renderFollowingFlows(slice: Sliceish): m.Children {
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
      flow: FlowPoint, dur: TPDuration,
      includeProcessName: boolean): m.Children {
    const sliceId = flow.sliceId;
    const trackId = globals.state.uiTrackIdByTraceTrackId[flow.trackId];
    const description = flow.sliceChromeCustomName === undefined ?
        flow.sliceName :
        flow.sliceChromeCustomName;
    const sliceLink = renderSliceLink(sliceId, trackId, description);
    const threadName = includeProcessName ?
        `${flow.threadName} (${flow.processName})` :
        flow.threadName;
    return m(
        TreeNode,
        {left: 'Flow'},
        m(TreeNode, {left: 'Slice', right: sliceLink}),
        m(TreeNode, {left: 'Delay', right: tpTimeToCode(dur)}),
        m(TreeNode, {left: 'Thread', right: threadName}),
    );
  }

  private renderArguments(slice: Sliceish): m.Children {
    if (slice.args && slice.args.size > 0) {
      const tree = convertArgsToTree(slice.args);
      return m(
          Section, {title: 'Arguments'}, m(Tree, renderArgTreeNodes(tree)));
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
              ({name, getAction}) =>
                  m(MenuItem, {label: name, onclick: getAction})),
      );
    } else {
      return undefined;
    }
  }
}
