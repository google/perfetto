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
import {Trace} from '../../public/trace';
import {BaseNodeData} from './node_types';
import {Button, ButtonVariant} from '../../widgets/button';

export interface SelectionNodeData extends BaseNodeData {
  readonly type: 'selection';
  // Snapshot of the selection at the time the node was created/updated.
  readonly ts: string; // bigint as string for serialization
  readonly dur: string;
}

export function createSelectionNode(
  id: string,
  x: number,
  y: number,
): SelectionNodeData {
  return {type: 'selection', id, x, y, ts: '0', dur: '0'};
}

export function renderSelectionNode(
  node: SelectionNodeData,
  updateNode: (
    updates: Partial<Omit<SelectionNodeData, 'type' | 'id'>>,
  ) => void,
  trace: Trace,
): m.Children {
  const timeSpan = trace.selection.getTimeSpanOfSelection();
  const hasSelection = timeSpan !== undefined;

  const snapButton = m(Button, {
    variant: ButtonVariant.Filled,
    onclick: () => {
      if (!timeSpan) return;
      updateNode({
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

  const hasCaptured = node.ts !== '0' || node.dur !== '0';
  const info = hasCaptured
    ? m(
        'span',
        {style: {fontSize: '11px', opacity: '0.7'}},
        `ts=${node.ts}, dur=${node.dur}`,
      )
    : m(
        'span',
        {style: {fontSize: '11px', opacity: '0.5'}},
        'Click snap to capture',
      );

  return m('.pf-qb-stack', [snapButton, info]);
}

// Build the SQL for a selection node: SELECT 0 AS id, <ts> AS ts, <dur> AS dur.
export function selectionNodeSql(node: SelectionNodeData): string {
  return `SELECT 0 AS id, ${node.ts} AS ts, ${node.dur} AS dur`;
}
