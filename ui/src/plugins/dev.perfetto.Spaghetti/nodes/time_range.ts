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
import {z} from 'zod';
import type {NodeManifest} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {Stack} from '../components/stack';

const TimeRangeConfigSchema = z
  .object({
    ts: z.string().describe('Start timestamp in ns (integer string)'),
    dur: z.string().describe('Duration in ns (integer string)'),
  })
  .describe(
    'Single-row (id, ts, dur) time window; at least one of ts or dur must not be "0".',
  );

export type TimeRangeConfig = z.infer<typeof TimeRangeConfigSchema>;

export const manifest: NodeManifest<TimeRangeConfig> = {
  title: 'Time Range',
  icon: 'highlight_alt',
  hue: 234,
  schema: TimeRangeConfigSchema,
  defaultConfig: () => ({ts: '0', dur: '0'}),
  isValid: (config) => config.ts !== '0' || config.dur !== '0',
  getOutputColumns() {
    return [
      {name: 'id', type: {kind: 'int'}},
      {name: 'ts', type: {kind: 'timestamp'}},
      {name: 'dur', type: {kind: 'duration'}},
    ];
  },
  generators: {
    sql: (config) => ({
      from: `(SELECT 0 AS id, ${config.ts} AS ts, ${config.dur} AS dur)`,
    }),
    psn: (config) => ({
      source: `FROM (SELECT 0 AS id, ${config.ts} AS ts, ${config.dur} AS dur)`,
    }),
  },
  render(config, updateConfig, ctx) {
    const timeSpan = ctx.trace.selection.getTimeSpanOfSelection();
    const hasSelection = timeSpan !== undefined;

    const snapButton = m(Button, {
      variant: ButtonVariant.Filled,
      onclick: () => {
        if (!timeSpan) return;
        updateConfig({
          ts: timeSpan.start.toString(),
          dur: timeSpan.duration.toString(),
        });
      },
      disabled: !hasSelection,
      label: 'Snap selection',
      title: hasSelection
        ? 'Capture current timeline selection'
        : 'Make a selection on the timeline first',
    });

    const hasCaptured = config.ts !== '0' || config.dur !== '0';
    const info = hasCaptured
      ? m('span.pf-spag-hint', `ts=${config.ts}, dur=${config.dur}`)
      : m('span.pf-spag-hint.pf-spag-hint--muted', 'Click snap to capture');

    return m(Stack, [snapButton, info]);
  },
};
