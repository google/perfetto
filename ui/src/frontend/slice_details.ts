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
import {exists} from '../base/utils';
import {SliceDetails} from '../trace_processor/sql_utils/slice';
import {Anchor} from '../widgets/anchor';
import {MenuItem, PopupMenu2} from '../widgets/menu';
import {Section} from '../widgets/section';
import {SqlRef} from '../widgets/sql_ref';
import {Tree, TreeNode} from '../widgets/tree';
import {
  BreakdownByThreadState,
  BreakdownByThreadStateTreeNode,
} from './sql/thread_state';
import {addSqlTableTab} from './sql_table_tab_interface';
import {DurationWidget} from './widgets/duration';
import {renderProcessRef} from './widgets/process';
import {renderThreadRef} from './widgets/thread';
import {Timestamp} from './widgets/timestamp';
import {getSqlTableDescription} from './widgets/sql/table/sql_table_registry';
import {assertExists} from '../base/logging';
import {Trace} from '../public/trace';

// Renders a widget storing all of the generic details for a slice from the
// slice table.
export function renderDetails(
  trace: Trace,
  slice: SliceDetails,
  durationBreakdown?: BreakdownByThreadState,
) {
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
              addSqlTableTab(trace, {
                table: assertExists(getSqlTableDescription('slice')),
                filters: [
                  {
                    op: (cols) => `${cols[0]} = ${sqliteString(slice.name)}`,
                    columns: ['name'],
                  },
                ],
              });
            },
          }),
        ),
      }),
      m(TreeNode, {
        left: 'Category',
        right:
          !slice.category || slice.category === '[NULL]'
            ? 'N/A'
            : slice.category,
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
          right: m(DurationWidget, {dur: slice.dur}),
        },
        exists(durationBreakdown) &&
          slice.dur > 0 &&
          m(BreakdownByThreadStateTreeNode, {
            data: durationBreakdown,
            dur: slice.dur,
          }),
      ),
      renderThreadDuration(slice),
      slice.thread &&
        m(TreeNode, {
          left: 'Thread',
          right: renderThreadRef(slice.thread),
        }),
      slice.process &&
        m(TreeNode, {
          left: 'Process',
          right: renderProcessRef(slice.process),
        }),
      slice.process &&
        exists(slice.process.uid) &&
        m(TreeNode, {
          left: 'User ID',
          right: slice.process.uid,
        }),
      slice.process &&
        slice.process.packageName &&
        m(TreeNode, {
          left: 'Package name',
          right: slice.process.packageName,
        }),
      slice.process &&
        exists(slice.process.versionCode) &&
        m(TreeNode, {
          left: 'Version code',
          right: slice.process.versionCode,
        }),
      m(TreeNode, {
        left: 'SQL ID',
        right: m(SqlRef, {table: 'slice', id: slice.id}),
      }),
    ),
  );
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
        m(DurationWidget, {dur: sliceInfo.threadDur}),
        threadDurFractionSuffix,
      ],
    });
  } else {
    return undefined;
  }
}
