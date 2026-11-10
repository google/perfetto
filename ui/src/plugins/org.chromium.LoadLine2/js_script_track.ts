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
import {Duration, Time} from '../../base/time';
import {getColorForSlice} from '../../components/colorizer';
import {formatDuration} from '../../components/time_utils';
import {SliceTrack, renderTooltip} from '../../components/tracks/slice_track';
import {DurationWidget} from '../../components/widgets/duration';
import {Timestamp} from '../../components/widgets/timestamp';
import type {Trace} from '../../public/trace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {GridLayout} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';

function renderNullableDuration(trace: Trace, dur: bigint | null): m.Children {
  return dur !== null
    ? m(DurationWidget, {trace, dur: Duration.fromRaw(dur)})
    : 'N/A';
}

export function registerJsScriptLifecycleTrack(
  trace: Trace,
  uri: string,
  src: string,
): void {
  trace.tracks.registerTrack({
    uri,
    description:
      'End-to-end JavaScript request, response, background parse, compile, and execute stages.',
    renderer: SliceTrack.create({
      trace,
      uri,
      dataset: new SourceDataset({
        src,
        schema: {
          id: NUM,
          ts: LONG,
          dur: LONG,
          depth: NUM,
          name: STR,
          page: STR,
          script_id: NUM,
          stage: STR,
          script_name: STR,
          url: STR_NULL,
          script_kind: STR,
          milestone_status: STR_NULL,
          is_streamed: NUM,
          not_streamed_reason: STR_NULL,
          thread_name: STR_NULL,
          request_dur: LONG_NULL,
          response_dur: LONG_NULL,
          bg_parse_dur: LONG_NULL,
          bg_parse_cpu_dur: LONG_NULL,
          bg_parse_wait_dur: LONG_NULL,
          compile_dur: LONG_NULL,
          execute_dur: LONG_NULL,
          end_to_end_dur: LONG_NULL,
          bg_parse_cpu_ratio: NUM_NULL,
        },
      }),
      sliceSubtitle: (row) =>
        `${row.script_kind} • ${row.milestone_status ?? 'unknown'}`,
      colorizer: (row) =>
        getColorForSlice(`${row.page}:${row.script_id}:${row.script_name}`, {
          stripTrailingDigits: false,
        }),
      fillRatio: (row) => row.bg_parse_cpu_ratio ?? 1,
      tooltip: (slice) =>
        renderTooltip(trace, slice, {
          extras: [
            m(
              'div',
              `${slice.row.script_kind} • ${slice.row.milestone_status ?? 'unknown'}`,
            ),
            slice.row.end_to_end_dur !== null &&
              m(
                'div',
                `End-to-end: ${formatDuration(trace, Duration.fromRaw(slice.row.end_to_end_dur))}`,
              ),
            slice.row.bg_parse_dur !== null &&
              m(
                'div',
                `BG parse: ${formatDuration(trace, Duration.fromRaw(slice.row.bg_parse_dur))}`,
              ),
          ],
        }),
      detailsPanel: (row) => ({
        render: () =>
          m(
            DetailsShell,
            {
              title: 'JS Script Lifecycle',
              description: `${row.page} • ${row.script_name} (${row.stage})`,
            },
            m(
              GridLayout,
              m(
                Section,
                {title: 'Script & Slice Details'},
                m(Tree, [
                  m(TreeNode, {left: 'Script name', right: row.script_name}),
                  m(TreeNode, {left: 'Active stage', right: row.stage}),
                  m(TreeNode, {left: 'Page', right: row.page}),
                  m(TreeNode, {left: 'Script kind', right: row.script_kind}),
                  m(TreeNode, {
                    left: 'Milestone cutoff',
                    right: row.milestone_status ?? 'unknown',
                  }),
                  m(TreeNode, {
                    left: 'Start time',
                    right: m(Timestamp, {trace, ts: Time.fromRaw(row.ts)}),
                  }),
                  m(TreeNode, {
                    left: 'Stage duration',
                    right: m(DurationWidget, {
                      trace,
                      dur: Duration.fromRaw(row.dur),
                    }),
                  }),
                  m(TreeNode, {
                    left: 'Thread',
                    right: row.thread_name ?? 'N/A',
                  }),
                  m(TreeNode, {
                    left: 'Streamed',
                    right:
                      row.is_streamed !== 0
                        ? 'Yes'
                        : `No (${row.not_streamed_reason ?? 'n/a'})`,
                  }),
                  row.url !== null &&
                    row.url !== '' &&
                    m(TreeNode, {left: 'URL', right: row.url}),
                ]),
              ),
              m(
                Section,
                {title: 'End-to-End Lifecycle Timings'},
                m(Tree, [
                  m(TreeNode, {
                    left: 'End-to-end duration',
                    right: renderNullableDuration(trace, row.end_to_end_dur),
                  }),
                  m(TreeNode, {
                    left: '1. Network request',
                    right: renderNullableDuration(trace, row.request_dur),
                  }),
                  m(TreeNode, {
                    left: '2. Network response',
                    right: renderNullableDuration(trace, row.response_dur),
                  }),
                  m(TreeNode, {
                    left: '3. Background parse',
                    right:
                      row.bg_parse_dur !== null
                        ? m(
                            'span',
                            m(DurationWidget, {
                              trace,
                              dur: Duration.fromRaw(row.bg_parse_dur),
                            }),
                            ' (Parsing: ',
                            m(DurationWidget, {
                              trace,
                              dur: Duration.fromRaw(
                                row.bg_parse_cpu_dur ?? Duration.ZERO,
                              ),
                            }),
                            ', Waiting: ',
                            m(DurationWidget, {
                              trace,
                              dur: Duration.fromRaw(
                                row.bg_parse_wait_dur ?? Duration.ZERO,
                              ),
                            }),
                            ')',
                          )
                        : 'N/A',
                  }),
                  m(TreeNode, {
                    left: '4. Compile',
                    right: renderNullableDuration(trace, row.compile_dur),
                  }),
                  m(TreeNode, {
                    left: '5. Execute',
                    right: renderNullableDuration(trace, row.execute_dur),
                  }),
                  m(TreeNode, {left: 'Script ID', right: row.script_id}),
                ]),
              ),
            ),
          ),
      }),
    }),
  });
}
