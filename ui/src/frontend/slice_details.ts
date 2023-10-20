// Copyright (C) 2023 The Android Open Source Project
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

import m from 'mithril';

import {BigintMath} from '../base/bigint_math';
import {sqliteString} from '../base/string_utils';
import {Duration, duration, time} from '../base/time';
import {exists} from '../base/utils';
import {Anchor} from '../widgets/anchor';
import {DurationWidget} from '../widgets/duration';
import {MenuItem, PopupMenu2} from '../widgets/menu';
import {Section} from '../widgets/section';
import {SqlRef} from '../widgets/sql_ref';
import {Tree, TreeNode} from '../widgets/tree';

import {addTab} from './bottom_tab';
import {globals} from './globals';
import {SliceDetails} from './sql/slice';
import {
  BreakdownByThreadState,
  BreakdownByThreadStateTreeNode,
} from './sql/thread_state';
import {SqlTableTab} from './sql_table/tab';
import {SqlTables} from './sql_table/well_known_tables';
import {getProcessName, getThreadName} from './thread_and_process_info';
import {Timestamp} from './widgets/timestamp';

function computeDuration(ts: time, dur: duration): m.Children {
  if (dur === -1n) {
    const minDuration = globals.state.traceTime.end - ts;
    return `${Duration.format(minDuration)} (Did not end)`;
  } else {
    return m(DurationWidget, {dur});
  }
}

// Renders a widget storing all of the generic details for a slice from the
// slice table.
export function renderDetails(
    slice: SliceDetails, durationBreakdown?: BreakdownByThreadState) {
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
          m(
              TreeNode,
              {
                left: 'Duration',
                right: computeDuration(slice.ts, slice.dur),
              },
              exists(durationBreakdown) && slice.dur > 0 &&
                  m(BreakdownByThreadStateTreeNode, {
                    data: durationBreakdown,
                    dur: slice.dur,
                  }),
              ),
          renderThreadDuration(slice),
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

function renderThreadDuration(sliceInfo: SliceDetails) {
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
