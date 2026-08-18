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
import {Time, type time} from '../../base/time';
import {formatDuration} from '../../components/time_utils';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {Trace} from '../../public/trace';
import {NUM_NULL, STR_NULL} from '../../trace_processor/query_result';
import {Button} from '../../widgets/button';
import {DetailsShell} from '../../widgets/details_shell';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {sampleCategoryLabel} from './sample_colors';

export interface FlamechartFrameInfo {
  // Node id in _callstack_spc_forest.
  readonly frameId: number;
  readonly name: string;
  readonly ts: time;
  readonly dur: bigint;
  readonly category: number;
  readonly sampleCount: number;
  readonly trackUri: string;
}

// Details panel for a run of a frame on the sampled-stacks flamechart:
// identifies the frame (mapping, category, source location) and offers
// selecting the run's time range for aggregate analysis.
export class FlamechartFrameDetailsPanel implements TrackEventDetailsPanel {
  private mapping?: string;
  private sourceLocation?: string;

  constructor(
    private readonly trace: Trace,
    private readonly info: FlamechartFrameInfo,
  ) {}

  async load(): Promise<void> {
    const result = await this.trace.engine.query(`
      select
        mp.name as mapping,
        f.source_file as sourceFile,
        f.line_number as lineNumber
      from _callstack_spc_forest f
      left join stack_profile_mapping mp on mp.id = f.mapping_id
      where f.id = ${this.info.frameId}
    `);
    const row = result.maybeFirstRow({
      mapping: STR_NULL,
      sourceFile: STR_NULL,
      lineNumber: NUM_NULL,
    });
    if (row === undefined) return;
    this.mapping = row.mapping ?? undefined;
    if (row.sourceFile !== null) {
      this.sourceLocation =
        row.lineNumber === null
          ? row.sourceFile
          : `${row.sourceFile}:${row.lineNumber}`;
    }
  }

  render(): m.Children {
    const {trace, info} = this;
    return m(
      DetailsShell,
      {
        title: info.name,
        description: `${info.sampleCount} samples · ${formatDuration(
          trace,
          info.dur,
        )} (sampled)`,
        buttons: m(Button, {
          label: 'Select time range',
          onclick: () => {
            trace.selection.selectArea({
              start: info.ts,
              end: Time.fromRaw(info.ts + info.dur),
              trackUris: [info.trackUri],
            });
          },
        }),
      },
      m(
        Section,
        {title: 'Frame'},
        m(
          Tree,
          m(TreeNode, {left: 'Name', right: info.name}),
          this.mapping !== undefined &&
            m(TreeNode, {left: 'Mapping', right: this.mapping}),
          m(TreeNode, {
            left: 'Category',
            right: sampleCategoryLabel(info.category),
          }),
          this.sourceLocation !== undefined &&
            m(TreeNode, {left: 'Source', right: this.sourceLocation}),
          m(TreeNode, {
            left: 'Start',
            right: m(Timestamp, {trace, ts: info.ts}),
          }),
          m(TreeNode, {
            left: 'Duration (sampled)',
            right: m(DurationWidget, {trace, dur: info.dur}),
          }),
          m(TreeNode, {left: 'Samples', right: `${info.sampleCount}`}),
        ),
      ),
    );
  }
}
