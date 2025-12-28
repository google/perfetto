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
import {Trace} from '../../public/trace';
import {TrackEventSelection} from '../../public/selection';
import {TrackEventDetailsPanelSection} from '../../components/details/thread_slice_details_tab';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {STR_NULL, NUM_NULL} from '../../trace_processor/query_result';

interface CallstackFrame {
  name: string;
  sourceFile?: string;
  lineNumber?: number;
}

export class CallstackDetailsSection implements TrackEventDetailsPanelSection {
  private beginCallstack?: CallstackFrame[];
  private endCallstack?: CallstackFrame[];

  constructor(private trace: Trace) {}

  async load(selection: TrackEventSelection): Promise<void> {
    const {eventId} = selection;

    // Query for both begin and end callstacks in parallel
    [this.beginCallstack, this.endCallstack] = await Promise.all([
      this.queryCallstack(eventId, 'callsite_id'),
      this.queryCallstack(eventId, 'end_callsite_id'),
    ]);
  }

  render(): m.Children {
    const sections: m.Children[] = [];

    if (this.beginCallstack) {
      sections.push(this.renderFrames(this.beginCallstack, 'Callstack'));
    }

    if (this.endCallstack) {
      sections.push(this.renderFrames(this.endCallstack, 'End Callstack'));
    }

    return sections.length > 0 ? sections : null;
  }

  private async queryCallstack(
    sliceId: number,
    argKey: string,
  ): Promise<CallstackFrame[] | undefined> {
    const callsiteColumn =
      argKey === 'callsite_id' ? 'callsite_id' : 'end_callsite_id';
    const result = await this.trace.engine.query(`
      INCLUDE PERFETTO MODULE callstacks.stack_profile;

      WITH callstack AS MATERIALIZED (
        SELECT
          id,
          parent_id,
          name,
          source_file,
          line_number
        FROM _callstacks_for_stack_profile_samples!((
          SELECT ${callsiteColumn} as callsite_id
          FROM __intrinsic_track_event_callstacks
          WHERE slice_id = ${sliceId}
            AND ${callsiteColumn} IS NOT NULL
        ))
        ORDER BY id
      )
      SELECT name, source_file AS sourceFile, line_number AS lineNumber
      FROM _graph_scan!(
        (
          SELECT parent_id AS source_node_id, id AS dest_node_id
          FROM callstack
          WHERE parent_id IS NOT NULL
        ),
        (SELECT id, 0 AS depth FROM callstack WHERE parent_id IS NULL),
        (depth),
        (SELECT t.id, t.depth + 1 AS depth FROM $table t)
      ) s
      JOIN callstack c USING (id)
      ORDER BY s.depth
    `);
    const frames: CallstackFrame[] = [];
    const it = result.iter({
      name: STR_NULL,
      sourceFile: STR_NULL,
      lineNumber: NUM_NULL,
    });
    for (; it.valid(); it.next()) {
      frames.push({
        name: it.name ?? '<unknown>',
        sourceFile: it.sourceFile ?? undefined,
        lineNumber: it.lineNumber ?? undefined,
      });
    }
    return frames.length === 0 ? undefined : frames;
  }

  private renderFrames(frames: CallstackFrame[], title: string): m.Children {
    return m(
      Section,
      {title},
      m(
        Tree,
        frames.map((frame, index) => {
          const location =
            frame.sourceFile && frame.lineNumber !== undefined
              ? `${frame.sourceFile}:${frame.lineNumber}`
              : frame.sourceFile ?? '';

          return m(TreeNode, {
            left: `#${index}`,
            right: m(
              'span',
              frame.name,
              location && m('span.note', ` (${location})`),
            ),
          });
        }),
      ),
    );
  }
}
