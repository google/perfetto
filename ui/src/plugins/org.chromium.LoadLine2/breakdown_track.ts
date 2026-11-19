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
import {sqliteString} from '../../base/string_utils';
import {Duration, type duration, Time} from '../../base/time';
import {SliceTrack, renderTooltip} from '../../components/tracks/slice_track';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

interface StageBreakdownItem {
  readonly label: string;
  readonly dur: duration;
  readonly percentage?: number;
}

export function registerBreakdownTrack(
  trace: Trace,
  uri: string,
  src: string,
  metric: 'visual' | 'interactive',
  description: string,
): void {
  trace.tracks.registerTrack({
    uri,
    description,
    renderer: SliceTrack.create({
      trace,
      uri,
      initialMaxDepth: 4,
      dataset: new SourceDataset({
        src,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          depth: NUM,
          name: STR,
          page: STR,
          stage_category: STR,
          details: STR,
        },
      }),
      tooltip: (slice) =>
        renderTooltip(trace, slice, {
          extras: m('div', slice.row.details.replaceAll('\n', ', ')),
        }),
      detailsPanel: (row) => {
        let stageBreakdownItems: StageBreakdownItem[] = [];

        return {
          load: async () => {
            if (row.stage_category !== 'top_stage') {
              return;
            }
            if (row.name === 'renderer') {
              const res = await trace.engine.query(`
                SELECT subsystem, dur, percentage
                FROM chrome_loadline2_renderer_breakdown
                WHERE page = ${sqliteString(row.page)} AND metric = '${metric}'
                ORDER BY dur DESC, subsystem;
              `);
              const items: StageBreakdownItem[] = [];
              for (
                const it = res.iter({
                  subsystem: STR,
                  dur: LONG,
                  percentage: NUM,
                });
                it.valid();
                it.next()
              ) {
                items.push({
                  label: it.subsystem,
                  dur: Duration.fromRaw(it.dur),
                  percentage: it.percentage,
                });
              }
              stageBreakdownItems = items;
            } else if (row.name === 'presentation') {
              const res = await trace.engine.query(`
                SELECT stage, dur
                FROM chrome_loadline2_presentation_breakdown
                WHERE page = ${sqliteString(row.page)}
                  AND metric = '${metric}'
                  AND stage_depth = 1
                ORDER BY ts, id;
              `);
              const items: StageBreakdownItem[] = [];
              for (
                const it = res.iter({stage: STR, dur: LONG});
                it.valid();
                it.next()
              ) {
                items.push({label: it.stage, dur: Duration.fromRaw(it.dur)});
              }
              stageBreakdownItems = items;
            }
          },
          render: () => {
            const breakdownItems =
              stageBreakdownItems.length > 0
                ? stageBreakdownItems.map((item) =>
                    m(TreeNode, {
                      left: item.label,
                      right:
                        item.percentage !== undefined
                          ? m(
                              'span',
                              m(DurationWidget, {trace, dur: item.dur}),
                              ` (${item.percentage.toFixed(1)}%)`,
                            )
                          : m(DurationWidget, {trace, dur: item.dur}),
                    }),
                  )
                : [m(TreeNode, {left: 'Summary', right: row.details})];

            return m(
              DetailsShell,
              {
                title: 'LoadLine 2 Breakdown Slice',
                description: `${row.page} • ${row.name}`,
              },
              m(
                GridLayout,
                m(
                  Section,
                  {title: 'Stage Details'},
                  m(Tree, [
                    m(TreeNode, {left: 'Name', right: row.name}),
                    m(TreeNode, {left: 'Page', right: row.page}),
                    m(TreeNode, {left: 'Category', right: row.stage_category}),
                    m(TreeNode, {
                      left: 'Start time',
                      right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
                    }),
                    m(TreeNode, {
                      left: 'Duration',
                      right: m(DurationWidget, {
                        trace,
                        dur: Duration.fromRaw(row.dur),
                      }),
                    }),
                    m(TreeNode, {left: 'Depth', right: row.depth}),
                  ]),
                ),
                m(
                  Section,
                  {title: 'Critical Path Breakdown'},
                  m(Tree, breakdownItems),
                ),
              ),
            );
          },
        };
      },
    }),
  });
}
