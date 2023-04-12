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
import {Arg, ArgsTree, isArgTreeArray, isArgTreeMap} from '../common/arg_types';
import {timeToCode} from '../common/time';

import {FlowPoint, globals, SliceDetails} from './globals';
import {PanelSize} from './panel';
import {PopupMenuButton, PopupMenuItem} from './popup_menu';
import {runQueryInNewTab} from './query_result_tab';
import {verticalScrollToTrack} from './scroll_helper';
import {SlicePanel} from './slice_panel';

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

function getSliceContextMenuItems(slice: SliceDetails): PopupMenuItem[] {
  return ITEMS.filter((item) => item.shouldDisplay(slice)).map((item) => {
    return {
      itemType: 'regular',
      text: item.name,
      callback: () => item.getAction(slice),
    };
  });
}

// Table row contents is one of two things:
// 1. Key-value pair
interface TableRow {
  kind: 'TableRow';
  key: string;
  value: Arg;

  // Whether it's an argument (from the `args` table) or whether it's a property
  // of the slice (i.e. `dur`, coming from `slice` table). Args have additional
  // actions associated with them.
  isArg: boolean;

  // A full key for the arguments displayed in a tree.
  full_key?: string;
}

// 2. Common prefix for values in an array
interface TableHeader {
  kind: 'TableHeader';
  header: string;
}

type RowContents = TableRow|TableHeader;

function isTableHeader(contents: RowContents): contents is TableHeader {
  return contents.kind === 'TableHeader';
}

function appendPrefix(p1: string, p2: string): string {
  if (p1.length === 0) {
    return p2;
  }
  return `${p1}.${p2}`;
}

interface Row {
  // How many columns (empty or with an index) precede a key
  indentLevel: number;
  // Optional tooltip to be displayed on the key. Used to display the full key,
  // which has to be reconstructed from the information that might not even be
  // visible on the screen otherwise.
  tooltip?: string;
  contents: RowContents;
}

class TableBuilder {
  // Row data generated by builder
  rows: Row[] = [];
  indentLevel = 0;

  // Maximum indent level of a key, used to determine total number of columns
  maxIndent = 0;

  // Add a key-value pair into the table
  add(key: string, value: Arg) {
    this.rows.push({
      indentLevel: 0,
      contents: {kind: 'TableRow', key, value, isArg: false},
    });
  }

  // Add arguments tree into the table
  addTree(tree: ArgsTree) {
    this.addTreeInternal(tree, '', '');
  }

  private addTreeInternal(
      record: ArgsTree, prefix: string, completePrefix: string) {
    if (isArgTreeArray(record)) {
      if (record.length === 1) {
        this.addTreeInternal(record[0], `${prefix}[0]`, `${completePrefix}[0]`);
        return;
      }

      // Add the current prefix as a separate row
      if (prefix.length > 0) {
        this.rows.push({
          indentLevel: this.indentLevel,
          contents: {kind: 'TableHeader', header: prefix},
          tooltip: completePrefix,
        });
      }

      this.indentLevel++;
      for (let i = 0; i < record.length; i++) {
        // Prefix is empty for array elements because we don't want to repeat
        // the common prefix
        this.addTreeInternal(record[i], `[${i}]`, `${completePrefix}[${i}]`);
      }
      this.indentLevel--;
    } else if (isArgTreeMap(record)) {
      const entries = Object.entries(record);
      if (entries.length === 1) {
        // Don't want to create a level of indirection in case object contains
        // only one value; think of it like file browser in IDEs not showing
        // intermediate nodes for common hierarchy corresponding to Java package
        // prefix (e.g. "com/google/perfetto").
        //
        // In this case, add key as a prefix part.
        const [key, value] = entries[0];
        this.addTreeInternal(
            value,
            appendPrefix(prefix, key),
            appendPrefix(completePrefix, key));
      } else {
        if (prefix.length > 0) {
          const row = this.indentLevel;
          this.rows.push({
            indentLevel: row,
            contents: {kind: 'TableHeader', header: prefix},
            tooltip: completePrefix,
          });
          this.indentLevel++;
        }
        for (const [key, value] of entries) {
          this.addTreeInternal(value, key, appendPrefix(completePrefix, key));
        }
        if (prefix.length > 0) {
          this.indentLevel--;
        }
      }
    } else {
      // Leaf value in the tree: add to the table
      const row = this.indentLevel;
      this.rows.push({
        indentLevel: row,
        contents: {
          kind: 'TableRow',
          key: prefix,
          value: record,
          full_key: completePrefix,
          isArg: true,
        },
        tooltip: completePrefix,
      });
    }
  }
}

export class ChromeSliceDetailsPanel extends SlicePanel {
  view() {
    const sliceInfo = globals.sliceDetails;
    if (sliceInfo.ts !== undefined && sliceInfo.dur !== undefined &&
        sliceInfo.name !== undefined) {
      const defaultBuilder = new TableBuilder();
      defaultBuilder.add('Name', sliceInfo.name);
      defaultBuilder.add(
          'Category',
          !sliceInfo.category || sliceInfo.category === '[NULL]' ?
              'N/A' :
              sliceInfo.category);
      defaultBuilder.add('Start time', timeToCode(sliceInfo.ts));
      if (sliceInfo.absTime !== undefined) {
        defaultBuilder.add('Absolute Time', sliceInfo.absTime);
      }
      defaultBuilder.add(
          'Duration', this.computeDuration(sliceInfo.ts, sliceInfo.dur));
      if (sliceInfo.threadTs !== undefined &&
          sliceInfo.threadDur !== undefined) {
        // If we have valid thread duration, also display a percentage of
        // |threadDur| compared to |dur|.
        const threadDurFractionSuffix = sliceInfo.threadDur === -1 ?
            '' :
            ` (${(sliceInfo.threadDur / sliceInfo.dur * 100).toFixed(2)}%)`;
        defaultBuilder.add(
            'Thread duration',
            this.computeDuration(sliceInfo.threadTs, sliceInfo.threadDur) +
                threadDurFractionSuffix);
      }

      for (const [key, value] of this.getProcessThreadDetails(sliceInfo)) {
        if (value !== undefined) {
          defaultBuilder.add(key, value);
        }
      }

      defaultBuilder.add(
          'Slice ID',
          (sliceInfo.id !== undefined) ? sliceInfo.id.toString() : 'Unknown');
      if (sliceInfo.description) {
        for (const [key, value] of sliceInfo.description) {
          defaultBuilder.add(key, value);
        }
      }
      return m(
          '.details-panel',
          m('.details-panel-heading', m('h2', `Slice Details`)),
          m('.details-table-multicolumn', [
            this.renderTable(defaultBuilder, '.half-width-panel'),
            this.renderRhs(sliceInfo),
          ]));
    } else {
      return m(
          '.details-panel',
          m('.details-panel-heading',
            m(
                'h2',
                `Slice Details`,
                )));
    }
  }

  private fillFlowPanel(
      name: string, flows: {flow: FlowPoint, dur: number}[],
      includeProcessName: boolean, result: Map<string, TableBuilder>) {
    if (flows.length === 0) return;

    const builder = new TableBuilder();
    for (const {flow, dur} of flows) {
      builder.add('Slice', {
        kind: 'SLICE',
        sliceId: flow.sliceId,
        trackId: globals.state.uiTrackIdByTraceTrackId[flow.trackId],
        description: flow.sliceChromeCustomName === undefined ?
            flow.sliceName :
            flow.sliceChromeCustomName,
      });
      builder.add('Delay', timeToCode(dur));
      builder.add(
          'Thread',
          includeProcessName ? `${flow.threadName} (${flow.processName})` :
                               flow.threadName);
    }
    result.set(name, builder);
  }

  renderCanvas(_ctx: CanvasRenderingContext2D, _size: PanelSize) {}

  fillArgs(slice: SliceDetails, builder: TableBuilder) {
    if (slice.argsTree && slice.args) {
      // Parsed arguments are available, need only to iterate over them to get
      // slice references
      for (const [key, value] of slice.args) {
        if (typeof value !== 'string') {
          builder.add(key, value);
        }
      }
      builder.addTree(slice.argsTree);
    } else if (slice.args) {
      // Parsing has failed, but arguments are available: display them in a flat
      // 2-column table
      for (const [key, value] of slice.args) {
        builder.add(key, value);
      }
    }
  }

  private getArgumentContextMenuItems(argument: TableRow): PopupMenuItem[] {
    if (argument.full_key === undefined) return [];
    if (typeof argument.value !== 'string') return [];
    const argValue: string = argument.value;

    const fullKey = argument.full_key;
    return [
      {
        itemType: 'regular',
        text: 'Copy full key',
        callback: () => {
          navigator.clipboard.writeText(fullKey);
        },
      },
      {
        itemType: 'regular',
        text: 'Find slices with the same arg value',
        callback: () => {
          runQueryInNewTab(
              `
              select slice.*
              from slice
              join args using (arg_set_id)
              where key=${sqliteString(fullKey)} and display_value=${
                  sqliteString(argValue)}
          `,
              `Arg: ${sqliteString(fullKey)}=${sqliteString(argValue)}`);
        },
      },
      {
        itemType: 'regular',
        text: 'Visualise argument values',
        callback: () => {
          globals.dispatch(Actions.addVisualisedArg({argName: fullKey}));
        },
      },
    ];
  }

  renderRhs(sliceInfo: SliceDetails): m.Vnode {
    const builders = new Map<string, TableBuilder>();

    const immediatelyPrecedingByFlowSlices = [];
    const immediatelyFollowingByFlowSlices = [];
    for (const flow of globals.connectedFlows) {
      if (flow.begin.sliceId === sliceInfo.id) {
        immediatelyFollowingByFlowSlices.push({flow: flow.end, dur: flow.dur});
      }
      if (flow.end.sliceId === sliceInfo.id) {
        immediatelyPrecedingByFlowSlices.push(
            {flow: flow.begin, dur: flow.dur});
      }
    }

    // This is Chrome-specific bits:
    const isRunTask = sliceInfo.name === 'ThreadControllerImpl::RunTask' ||
        sliceInfo.name === 'ThreadPool_RunTask';
    const isPostTask = sliceInfo.name === 'ThreadPool_PostTask' ||
        sliceInfo.name === 'SequenceManager PostTask';

    // RunTask and PostTask are always same-process, so we can skip
    // emitting process name for them.
    this.fillFlowPanel(
        'Preceding flows',
        immediatelyPrecedingByFlowSlices,
        !isRunTask,
        builders);
    this.fillFlowPanel(
        'Following flows',
        immediatelyFollowingByFlowSlices,
        !isPostTask,
        builders);

    const argsBuilder = new TableBuilder();
    this.fillArgs(sliceInfo, argsBuilder);
    builders.set('Arguments', argsBuilder);

    const rows: m.Vnode<any, any>[] = [];
    for (const [name, builder] of builders) {
      rows.push(m('h3', name));
      rows.push(this.renderTable(builder));
    }

    const contextMenuItems = getSliceContextMenuItems(sliceInfo);
    if (contextMenuItems.length > 0) {
      rows.push(
          m(PopupMenuButton,
            {
              icon: 'arrow_drop_down',
              items: contextMenuItems,
            },
            'Contextual Options'));
    }

    return m('.half-width-panel', rows);
  }

  renderTable(builder: TableBuilder, additionalClasses: string = ''): m.Vnode {
    const rows: m.Vnode[] = [];
    for (const row of builder.rows) {
      const renderedRow: m.Vnode[] = [];
      const paddingLeft = `${row.indentLevel * 20}px`;
      if (isTableHeader(row.contents)) {
        renderedRow.push(
            m('th',
              {
                colspan: 2,
                title: row.tooltip,
                style: {'padding-left': paddingLeft},
              },
              row.contents.header));
      } else {
        const contents: any[] = [row.contents.key];
        if (row.contents.isArg) {
          contents.push(
              m('span.context-wrapper', m.trust('&nbsp;'), m(PopupMenuButton, {
                  icon: 'arrow_drop_down',
                  items: this.getArgumentContextMenuItems(row.contents),
                })));
        }

        renderedRow.push(
            m('th',
              {title: row.tooltip, style: {'padding-left': paddingLeft}},
              contents));
        const value = row.contents.value;
        if (typeof value === 'string') {
          renderedRow.push(m('td.value', this.mayLinkify(value)));
        } else {
          // Type of value being a record is not propagated into the callback
          // for some reason, extracting necessary parts as constants instead.
          const sliceId = value.sliceId;
          const trackId = value.trackId;
          renderedRow.push(
              m('td',
                m('i.material-icons.grey',
                  {
                    onclick: () => {
                      globals.makeSelection(Actions.selectChromeSlice(
                          {id: sliceId, trackId, table: 'slice'}));
                      // Ideally we want to have a callback to
                      // findCurrentSelection after this selection has been
                      // made. Here we do not have the info for horizontally
                      // scrolling to ts.
                      verticalScrollToTrack(trackId, true);
                    },
                    title: 'Go to destination slice',
                  },
                  'call_made'),
                value.description));
        }
      }

      rows.push(m('tr', renderedRow));
    }

    return m(`table.auto-layout${additionalClasses}`, rows);
  }

  private mayLinkify(what: string): string|m.Vnode {
    if (what.startsWith('http://') || what.startsWith('https://')) {
      return m('a', {href: what, target: '_blank'}, what);
    }
    return what;
  }
}
