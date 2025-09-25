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
import {DatasetSchema, SourceDataset} from '../../trace_processor/dataset';
import {durationFromSql, timeFromSql} from '../../trace_processor/query_result';
import {sqlValueToReadableString} from '../../trace_processor/sql_utils';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {DurationWidget} from '../widgets/duration';
import {Timestamp} from '../widgets/timestamp';

/**
 * Default details panel for DatasetSliceTrack that displays all fields from
 * the dataset query.
 *
 * This panel provides a "better than nothing" experience when no custom
 * details panel is specified. It automatically shows:
 * - Common slice fields (name, ts, dur) with appropriate formatting
 * - All other dataset columns as readable strings
 */
export class DatasetSliceTrackDetailsPanel<T extends DatasetSchema>
  implements TrackEventDetailsPanel
{
  constructor(
    private readonly trace: Trace,
    private readonly dataset: SourceDataset<T>,
    private readonly data: T,
  ) {}

  render() {
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
            // Special handling for common slice fields
            'id' in this.data &&
              m(TreeNode, {
                left: 'ID',
                right: sqlValueToReadableString(this.data.id),
              }),
            'name' in this.data &&
              m(TreeNode, {
                left: 'Name',
                right: sqlValueToReadableString(this.data.name),
              }),
            'ts' in this.data &&
              m(TreeNode, {
                left: 'Start time',
                right: m(Timestamp, {
                  trace: this.trace,
                  ts: timeFromSql(this.data.ts),
                }),
              }),
            'dur' in this.data &&
              m(TreeNode, {
                left: 'Duration',
                right: m(DurationWidget, {
                  trace: this.trace,
                  dur: durationFromSql(this.data.dur),
                }),
              }),
            'depth' in this.data &&
              m(TreeNode, {
                left: 'Depth',
                right: sqlValueToReadableString(this.data.depth),
              }),
            'layer' in this.data &&
              m(TreeNode, {
                left: 'Layer',
                right: sqlValueToReadableString(this.data.layer),
              }),
            // All other fields from the dataset's schema
            ...Object.keys(this.dataset.schema)
              .filter(
                (key) =>
                  !['id', 'name', 'ts', 'dur', 'depth', 'layer'].includes(key),
              )
              .map((key) =>
                m(TreeNode, {
                  left: key,
                  right: sqlValueToReadableString(this.data[key]),
                }),
              ),
          ]),
        ),
      ),
    );
  }
}
