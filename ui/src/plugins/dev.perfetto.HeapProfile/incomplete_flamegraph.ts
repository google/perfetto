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
import type {Trace} from '../../public/trace';
import {NUM} from '../../trace_processor/query_result';
import {Modal} from '../../widgets/modal';

// Whether the trace contains a heap graph that was not fully finalized, i.e.
// the flamegraph built from it would be incomplete.
export async function isHeapGraphIncomplete(trace: Trace): Promise<boolean> {
  const it = await trace.engine.query(`
    select value
    from stats
    where name = 'heap_graph_non_finalized_graph'
  `);
  return it.firstRow({value: NUM}).value > 0;
}

// The warning modal gating an incomplete heap-graph flamegraph. `onSkip` is
// invoked when the user chooses to view the partial graph anyway.
export function incompleteFlamegraphModal(
  trace: Trace,
  onSkip: () => void,
): m.Children {
  return m(Modal, {
    title: 'The flamegraph is incomplete',
    vAlign: 'TOP',
    content: m(
      'div',
      'The current trace does not have a fully formed flamegraph',
    ),
    buttons: [
      {
        text: 'Show the errors',
        primary: true,
        action: () => trace.navigate('#!/info'),
      },
      {
        text: 'Skip',
        action: onSkip,
      },
    ],
  });
}
