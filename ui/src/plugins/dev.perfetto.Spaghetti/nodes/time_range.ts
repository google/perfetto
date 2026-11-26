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
import {classNames} from '../../../base/classnames';
import {Duration} from '../../../base/time';
import type {NodeManifest} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';
import {TextInput} from '../../../widgets/text_input';
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

const INT_RE = /^-?\d+$/;

function isValidInt(s: string): boolean {
  return INT_RE.test(s.trim());
}

// The default (0, 0) means no range has been captured yet.
function isValidRange(config: TimeRangeConfig): boolean {
  if (!isValidInt(config.ts) || !isValidInt(config.dur)) return false;
  return config.ts.trim() !== '0' || config.dur.trim() !== '0';
}

function formatDurHint(durStr: string): string | undefined {
  if (!isValidInt(durStr)) return undefined;
  try {
    const dur = BigInt(durStr.trim());
    if (dur <= 0n) return undefined;
    return Duration.humanise(Duration.fromRaw(dur));
  } catch {
    return undefined;
  }
}

export const manifest: NodeManifest<TimeRangeConfig> = {
  title: 'Time Range',
  icon: 'highlight_alt',
  hue: 234,
  schema: TimeRangeConfigSchema,
  defaultConfig: () => ({ts: '0', dur: '0'}),
  render(config, updateConfig, ctx) {
    const timeSpan = ctx.trace.selection.getTimeSpanOfSelection();
    const hasSelection = timeSpan !== undefined;
    const {start: traceStart, end: traceEnd} = ctx.trace.traceInfo;
    const traceDur = traceEnd - traceStart;

    const tsValid = isValidInt(config.ts);
    const durValid = isValidInt(config.dur);
    const durHint = formatDurHint(config.dur);

    return m(Stack, {minWidth: 'sm'}, [
      m('.pf-spag-port-stepper', [
        m(Button, {
          variant: ButtonVariant.Filled,
          icon: 'ads_click',
          disabled: !hasSelection,
          label: 'Selection',
          title: hasSelection
            ? 'Capture current timeline selection'
            : 'Make a selection on the timeline first',
          onclick: () => {
            if (!timeSpan) return;
            updateConfig({
              ts: timeSpan.start.toString(),
              dur: timeSpan.duration.toString(),
            });
          },
        }),
        m(Button, {
          variant: ButtonVariant.Filled,
          icon: 'fit_screen',
          label: 'Full trace',
          title: 'Capture full trace time bounds',
          onclick: () => {
            updateConfig({
              ts: traceStart.toString(),
              dur: (traceDur > 0n ? traceDur : 0n).toString(),
            });
          },
        }),
      ]),
      m('.pf-spag-field-grid', [
        m('span.pf-spag-field-grid__label', 'ts (ns)'),
        m(TextInput, {
          value: config.ts,
          placeholder: '0',
          className: classNames(!tsValid && 'pf-spag-input--invalid'),
          onChange: (value: string) => updateConfig({ts: value.trim() || '0'}),
        }),
        m('span.pf-spag-field-grid__label', 'dur (ns)'),
        m(TextInput, {
          value: config.dur,
          placeholder: '0',
          className: classNames(!durValid && 'pf-spag-input--invalid'),
          onChange: (value: string) => updateConfig({dur: value.trim() || '0'}),
        }),
      ]),
      durHint !== undefined
        ? m('span.pf-spag-hint', `Window: ${durHint}`)
        : m(
            'span.pf-spag-hint.pf-spag-hint--muted',
            'Snap a range or enter ns values',
          ),
    ]);
  },
  getOutputColumns() {
    return [
      {name: 'id', type: {kind: 'int'}},
      {name: 'ts', type: {kind: 'timestamp'}},
      {name: 'dur', type: {kind: 'duration'}},
    ];
  },
  generators: {
    sql: (config) =>
      !isValidRange(config)
        ? undefined
        : {
            from: `(SELECT 0 AS id, ${config.ts.trim()} AS ts, ${config.dur.trim()} AS dur)`,
          },
    psn: (config) =>
      !isValidRange(config)
        ? undefined
        : {
            subquery: `(SELECT 0 AS id, ${config.ts.trim()} AS ts, ${config.dur.trim()} AS dur)`,
          },
  },
};
