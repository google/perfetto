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

import {Time} from '../../base/time';
import {extensions} from '../../components/extensions';
import {SLICE_TABLE} from '../../components/widgets/sql/table_definitions';
import type {Trace} from '../../public/trace';
import {LONG, NUM} from '../../trace_processor/query_result';
import type {TreeExplorerOptionalAction} from '../../widgets/tree_explorer';
import type {StitchedSelection} from './metrics';

// Parses the comma separated slice ids a merged node carries (the
// CONCAT_WITH_COMMA aggregation of `slice_ids`) into unique slice ids.
export function parseSliceIds(raw: string | undefined): number[] {
  if (raw === undefined) return [];
  const ids = new Set<number>();
  for (const part of raw.split(',')) {
    const trimmed = part.trim();
    if (trimmed === '') continue;
    const id = Number(trimmed);
    if (Number.isSafeInteger(id) && id >= 0) ids.add(id);
  }
  return [...ids];
}

// The drill downs offered on every stitched node: zoom the timeline to the
// slice the node stands for, or list all the slices merged into it. A hop
// frame stands for the slices it leads into, a callstack frame for the slices
// it was sampled in. Neither action changes the selection, so the flamegraph
// stays open to come back to.
export function stitchedNodeActions(
  trace: Trace,
  selection: StitchedSelection,
): ReadonlyArray<TreeExplorerOptionalAction> {
  return [
    {
      name: 'Go to slice',
      icon: 'near_me',
      category: 'DRILL',
      description:
        'Scroll and zoom the timeline to the slice (the one with the most ' +
        'time in the selection if the node merges several), keeping the ' +
        'selection and this flamegraph.',
      execute: async ({properties}) => {
        const ids = parseSliceIds(properties.get('slice_ids'));
        if (ids.length === 0) return;
        const res = await trace.engine.query(`
          select id, ts, dur
          from slice
          where id in (${ids.join(',')})
          order by
            min(iif(dur < 0, ${selection.end}, ts + dur), ${selection.end})
              - max(ts, ${selection.start}) desc,
            id
          limit 1
        `);
        const slice = res.firstRow({id: NUM, ts: LONG, dur: LONG});
        const [event] = await trace.selection.resolveSqlEvents('slice', [
          slice.id,
        ]);
        const start = Time.fromRaw(slice.ts);
        trace.scrollTo({
          track:
            event === undefined
              ? undefined
              : {uri: event.trackUri, expandGroup: true},
          // Same framing as selecting the slice would do.
          time:
            slice.dur > 0n
              ? {
                  start,
                  end: Time.fromRaw(slice.ts + slice.dur),
                  behavior: 'focus',
                }
              : {start, behavior: 'focus'},
        });
      },
    },
    {
      name: 'Show slices in table',
      icon: 'table_chart',
      category: 'DRILL',
      description: 'List all the slices merged into this node.',
      execute: ({properties}) => {
        const ids = parseSliceIds(properties.get('slice_ids'));
        if (ids.length === 0) return;
        extensions.addLegacySqlTableTab(trace, {
          table: SLICE_TABLE,
          filters: [
            {
              op: (cols) => `${cols[0]} in (${ids.join(',')})`,
              columns: ['id'],
            },
          ],
        });
      },
    },
  ];
}
