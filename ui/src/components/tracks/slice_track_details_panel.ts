// Copyright (C) 2025 The Android Open Source Project
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
import {TrackEventDetailsPanel} from '../../public/details_panel';
import {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {sqlValueToReadableString} from '../../trace_processor/sql_utils';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {DurationWidget} from '../widgets/duration';
import {Timestamp} from '../widgets/timestamp';
import {RowSchema} from './slice_track';
import {exists} from '../../base/utils';
import {Time} from '../../base/time';

/**
 * Default details panel for SliceTrack that displays all fields from
 * the dataset query.
 *
 * This panel provides a "better than nothing" experience when no custom
 * details panel is specified. It automatically shows:
 * - Common slice fields (name, ts, dur) with appropriate formatting
 * - All other dataset columns as readable strings
 */
export class SliceTrackDetailsPanel<T extends RowSchema>
  implements TrackEventDetailsPanel
{
  constructor(
    private readonly trace: Trace,
    private readonly dataset: SourceDataset<T>,
    private readonly data: T,
  ) {}

  render() {
    const data = this.data;

    return m(
      DetailsShell,
      {
        title: 'Slice',
      },
      m(
        GridLayout,
        m(
          Section,
          {title: 'Details'},
          m(Tree, [
            // Special handling for well-known slice fields
            exists(data.id) &&
              m(TreeNode, {
                left: 'ID',
                right: data.id,
              }),
            exists(data.ts) &&
              m(TreeNode, {
                left: 'Start time',
                right: m(Timestamp, {
                  trace: this.trace,
                  ts: Time.fromRaw(data.ts),
                }),
              }),
            exists(data.dur) &&
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {
                  trace: this.trace,
                  dur: data.dur,
                }),
              }),
            // List all other fields from the dataset's schema
            ...Object.keys(this.dataset.schema)
              .filter((key) => !['id', 'ts', 'dur'].includes(key))
              .map((key) => {
                const value = data[key];
                return m(TreeNode, {
                  left: key,
                  right: sqlValueToReadableString(value),
                });
              }),
          ]),
        ),
      ),
    );
  }
}
