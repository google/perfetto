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

import {describe, expect, test, vi} from 'vitest';
import m from 'mithril';
import protos from '../../protos';
import {createFakeTraceImpl} from '../../core/fake_trace_impl';
import {
  deserializeTrackNode,
  serializeTrackNode,
} from '../../core/state_serialization';
import type {TraceImpl} from '../../core/trace_impl';
import type {TrackTags} from '../../public/track';
import {TrackNode} from '../../public/workspace';
import {createQueryResult} from '../../trace_processor/query_result';
import {TrackView} from './track_view';

async function setup(
  tags: TrackTags = {trackIds: [1], dimensionSourceTrackIds: [1]},
) {
  const T = protos.QueryResult.CellsBatch.CellType;
  const result = createQueryResult({query: 'dimensions'});
  result.appendResultBatch(
    protos.QueryResult.encode({
      columnNames: [
        'trackId',
        'name',
        'intValue',
        'stringValue',
        'displayName',
        'displayValue',
      ],
      batch: [
        {
          cells: [
            T.CELL_VARINT,
            T.CELL_STRING,
            T.CELL_VARINT,
            T.CELL_NULL,
            T.CELL_NULL,
            T.CELL_STRING,
            T.CELL_VARINT,
            T.CELL_STRING,
            T.CELL_VARINT,
            T.CELL_NULL,
            T.CELL_NULL,
            T.CELL_STRING,
          ],
          varintCells: [1, 3, 2, 7],
          stringCells: ['rank', '3', 'rank', '7'].join('\0'),
          isLastBatch: true,
        },
      ],
    }).finish(),
  );
  const trace = createFakeTraceImpl();
  vi.spyOn(trace.engine, 'query').mockResolvedValue(result);
  await trace.trackDimensionIndex.initialize();
  trace.tracks.registerTrack({
    uri: 'counter',
    tags,
    renderer: {render: () => {}, getHeight: () => 18},
  });
  const node = new TrackNode({
    uri: 'counter',
    name: 'Counter',
    chips: ['debuggable'],
    subtitle: 'example.com',
  });
  trace.defaultWorkspace.addChildLast(node);
  return {trace, node};
}

function render(trace: TraceImpl, node: TrackNode) {
  const view = new TrackView(trace, node, 0);
  const dom = view.renderDOM(
    {
      lite: false,
      depth: 0,
      stickyTop: 0,
      collapsible: false,
      onTrackMouseOver: () => {},
      onTrackMouseOut: () => {},
    },
    [],
  );
  expect(dom.attrs.heightPx).toBe(18);
  const root = document.createElement('div');
  m.render(root, dom);
  expect(root.querySelector('.pf-track__subtitle')?.getAttribute('title')).toBe(
    node.subtitle,
  );
  const chips = root.querySelector(
    '.pf-track__menubar .pf-track__chips',
  )?.textContent;
  m.render(root, []);
  return chips;
}

describe('derived dimension chips', () => {
  test('render on an 18px non-TrackEvent track without changing its annotations', async () => {
    const {trace, node} = await setup();
    expect(render(trace, node)).toContain('rank 3');
    expect(render(trace, node)).toContain('debuggable');
    expect(node.chips).toEqual(['debuggable']);
    expect(node.subtitle).toBe('example.com');
  });

  test('are recomputed after pinning and serialization, not persisted', async () => {
    const {trace, node} = await setup();
    trace.defaultWorkspace.pinTrack(node);
    const pinned = trace.defaultWorkspace.pinnedTracksNode.children[0]!;
    expect(render(trace, pinned)).toContain('rank 3');
    const state = serializeTrackNode(pinned);
    expect(state.chips).toEqual(['debuggable']);
    expect(state.subtitle).toBe('example.com');
    expect(JSON.stringify(state)).not.toContain('rank 3');
    const restored = deserializeTrackNode(JSON.parse(JSON.stringify(state)));
    expect(render(trace, restored)).toContain('rank 3');
    expect(restored.chips).toEqual(['debuggable']);
  });

  test('summary sources override event lookup IDs', async () => {
    const {trace, node} = await setup({
      trackIds: [1],
      dimensionSourceTrackIds: [2],
    });
    expect(render(trace, node)).toContain('rank 7');
    expect(trace.tracks.getTrack('counter')?.tags?.trackIds).toEqual([1]);
  });

  test('a registered track does not gain chips without explicit sources', async () => {
    const {trace, node} = await setup({trackIds: [1]});
    expect(render(trace, node)).not.toContain('rank 3');
    expect(render(trace, node)).toContain('debuggable');
  });

  test('an empty source override does not use event IDs', async () => {
    const {trace, node} = await setup({
      trackIds: [1],
      dimensionSourceTrackIds: [],
    });
    expect(render(trace, node)).not.toContain('rank 3');
  });
});
