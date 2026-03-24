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
import {NodeManifest} from '../node_types';
import {Button, ButtonVariant} from '../../../widgets/button';

export interface TimeRangeConfig {
  readonly ts: string; // bigint as string for serialization
  readonly dur: string;
}

export const manifest: NodeManifest<TimeRangeConfig> = {
  title: 'Time Range',
  icon: 'highlight_alt',
  outputs: [{name: 'output', content: 'Output', direction: 'right'}],
  canDockBottom: true,
  hue: 15,
  defaultConfig: () => ({ts: '0', dur: '0'}),
  isValid: (config) => config.ts !== '0' || config.dur !== '0',
  getOutputColumns() {
    return [
      {name: 'id', type: {kind: 'int'}},
      {name: 'ts', type: {kind: 'timestamp'}},
      {name: 'dur', type: {kind: 'duration'}},
    ];
  },
  emitIr(config) {
    return {sql: `SELECT 0 AS id, ${config.ts} AS ts, ${config.dur} AS dur`};
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
      ? m(
          'span',
          {style: {fontSize: '11px', opacity: '0.7'}},
          `ts=${config.ts}, dur=${config.dur}`,
        )
      : m(
          'span',
          {style: {fontSize: '11px', opacity: '0.5'}},
          'Click snap to capture',
        );

    return m('.pf-qb-stack', [snapButton, info]);
  },
};
