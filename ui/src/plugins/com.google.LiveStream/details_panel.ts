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
import {AsyncMemo} from '../../base/async_memo';
import {type duration, Time, type time} from '../../base/time';
import type {TrackEventDetailsPanel} from '../../public/details_panel';
import type {Engine} from '../../trace_processor/engine';
import {LONG, NUM, STR} from '../../trace_processor/query_result';
import {DetailsShell} from '../../widgets/details_shell';
import {EmptyState} from '../../widgets/empty_state';
import {GridLayout, GridLayoutColumn} from '../../widgets/grid_layout';
import {Section} from '../../widgets/section';
import {Tree, TreeNode} from '../../widgets/tree';
import {ACTIVE_DUR, SLICE_TABLE, sliceArgs} from './store';
import {formatWallTimestamp, type WallClock} from './wall_clock';

const TITLE = 'Live Stream Slice';

/** What the panel reads of the session at render time. */
export interface SliceTrackInfo {
  // Generation of the latest change to the track's rows.
  gen(): number;
  // Undefined before the first data.
  clock(): WallClock | undefined;
  // The latest data's time: active slices last until then so far.
  playhead(): time;
}

interface SliceDetails {
  readonly name: string;
  readonly category: string;
  readonly ts: time;
  // ACTIVE_DUR while active.
  readonly dur: duration;
  readonly depth: number;
  readonly args: ReadonlyArray<{readonly key: string; readonly value: string}>;
}

/**
 * Details of a live slice. The row is reloaded whenever its track changes,
 * because active slices complete and old slices are pruned while the panel is
 * open.
 */
export class LiveSliceDetailsPanel implements TrackEventDetailsPanel {
  private readonly memo = new AsyncMemo<SliceDetails | undefined>();

  constructor(
    private readonly engine: Engine,
    private readonly id: number,
    private readonly track: SliceTrackInfo,
  ) {}

  render(): m.Children {
    const result = this.memo.use({
      key: {id: this.id, gen: this.track.gen()},
      compute: () => loadSlice(this.engine, this.id),
      retainOn: ['gen'],
    });
    const slice = result.data;
    if (slice === undefined) {
      const title = result.isPending ? 'Loading…' : 'No longer retained';
      return m(DetailsShell, {title: TITLE}, m(EmptyState, {title}));
    }

    // Active slices last until the playhead so far.
    const active = slice.dur === ACTIVE_DUR;
    const playhead = this.track.playhead();
    const dur = active
      ? playhead > slice.ts
        ? playhead - slice.ts
        : 0n
      : slice.dur;
    const durText = dur === 0n ? '0 ms (Instant)' : formatMs(dur);
    const clock = this.track.clock();
    const end = formatWallTimestamp(Time.add(slice.ts, dur), clock);

    return m(
      DetailsShell,
      {title: TITLE, description: slice.name},
      m(
        GridLayout,
        m(
          GridLayoutColumn,
          m(
            Section,
            {title: 'Slice Summary'},
            m(
              Tree,
              m(TreeNode, {left: 'Name', right: slice.name}),
              m(TreeNode, {left: 'Category', right: slice.category}),
              m(TreeNode, {
                left: 'State',
                right: active ? 'ACTIVE' : dur === 0n ? 'INSTANT' : 'COMPLETED',
              }),
              m(TreeNode, {
                left: 'Start Time',
                right: formatWallTimestamp(slice.ts, clock),
              }),
              dur > 0n &&
                m(TreeNode, {
                  left: 'End Time',
                  right: active ? `${end} (In-Flight)` : end,
                }),
              m(TreeNode, {left: 'Duration', right: durText}),
              m(TreeNode, {left: 'Depth', right: String(slice.depth)}),
            ),
          ),
        ),
        slice.args.length > 0 &&
          m(
            GridLayoutColumn,
            m(
              Section,
              {title: 'Metadata & Details'},
              m(
                Tree,
                slice.args.map(({key, value}) =>
                  m(TreeNode, {left: key, right: value}),
                ),
              ),
            ),
          ),
      ),
    );
  }
}

// E.g. "12.345 ms".
function formatMs(dur: duration): string {
  return `${(Number(dur) / 1e6).toFixed(3)} ms`;
}

async function loadSlice(
  engine: Engine,
  id: number,
): Promise<SliceDetails | undefined> {
  const slice = await engine.query(`
    SELECT name, category, ts, dur, depth FROM ${SLICE_TABLE} WHERE id = ${id}
  `);
  const row = slice.maybeFirstRow({
    name: STR,
    category: STR,
    ts: LONG,
    dur: LONG,
    depth: NUM,
  });
  if (row === undefined) return undefined;

  const args = [];
  const argRows = await engine.query(sliceArgs(id));
  for (const it = argRows.iter({key: STR, value: STR}); it.valid(); it.next()) {
    args.push({key: it.key, value: it.value});
  }
  return {
    name: row.name,
    category: row.category,
    ts: Time.fromRaw(row.ts),
    dur: row.dur,
    depth: row.depth,
    args,
  };
}
