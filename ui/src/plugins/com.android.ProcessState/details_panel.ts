// Copyright (C) 2026 The Android Open Source Project
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
import {Icons} from '../../base/semantic_icons';
import {Time} from '../../base/time';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {Trace} from '../../public/trace';
import type {DatasetSchema} from '../../trace_processor/dataset';
import type {Row} from '../../trace_processor/query_result';
import {sqlValueToReadableString} from '../../trace_processor/sql_utils';
import {Anchor} from '../../widgets/anchor';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

export interface ProcessStateRow extends Row {
  readonly id: number;
  readonly ts: bigint;
  readonly dur: bigint;
}

/** Lists the columns of `schema` for one process state slice. */
export class ProcessStateDetailsPanel implements TrackEventDetailsPanel {
  constructor(
    private readonly trace: Trace,
    private readonly schema: DatasetSchema,
    private readonly row: ProcessStateRow,
    // Set on the overview row: links to the same slice on the owner track.
    private readonly ownerUri?: string,
  ) {}

  render() {
    const {trace, row, ownerUri} = this;
    return m(
      DetailsShell,
      {title: 'Slice'},
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(Tree, [
            m(TreeNode, {left: 'ID', right: row.id}),
            m(TreeNode, {
              left: 'Start time',
              right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
            }),
            m(TreeNode, {
              left: 'Duration',
              right: m(DurationWidget, {trace, dur: row.dur}),
            }),
            ...Object.keys(this.schema)
              .filter((key) => !['id', 'ts', 'dur'].includes(key))
              .map((key) => {
                const text = sqlValueToReadableString(row[key]);
                return m(TreeNode, {
                  left: key,
                  right:
                    key === 'process_name' && ownerUri !== undefined
                      ? m(
                          Anchor,
                          {
                            icon: Icons.UpdateSelection,
                            title: 'Show in process group',
                            onclick: () =>
                              trace.selection.selectTrackEvent(
                                ownerUri,
                                row.id,
                                {scrollToSelection: true},
                              ),
                          },
                          text,
                        )
                      : text,
                });
              }),
          ]),
        ),
      ),
    );
  }
}
