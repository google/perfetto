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
import protos from '../../protos';
import type {SelectionOpts, TrackEventSelection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {
  createQueryResult,
  type QueryResult,
} from '../../trace_processor/query_result';
import {VideoFrameLinkSection} from './video_frame_link';

const CELL_TYPE = protos.QueryResult.CellsBatch.CellType;

// Builds the (id) query result the section expects, with every cell encoded as
// a varint.
function queryResult(ids: ReadonlyArray<number>): QueryResult {
  const batch = protos.QueryResult.CellsBatch.create({
    cells: ids.map(() => CELL_TYPE.CELL_VARINT),
    varintCells: [...ids],
    isLastBatch: true,
  });
  const result = createQueryResult({query: 'fake'});
  result.appendResultBatch(
    protos.QueryResult.encode(
      protos.QueryResult.create({
        columnNames: ['id'],
        batch: [batch],
      }),
    ).finish(),
  );
  return result;
}

interface ResolvedEvent {
  readonly trackUri: string;
  readonly eventId: number;
}

interface SelectedEvent extends ResolvedEvent {
  readonly opts?: SelectionOpts;
}

// A trace whose engine answers any query with `ids`, recording the queries it
// runs, the SQL events it resolves, and the track events it selects.
function fakeTrace(ids: ReadonlyArray<number>, resolved?: ResolvedEvent) {
  const queries: string[] = [];
  const resolvedArgs: Array<{
    readonly table: string;
    readonly ids: ReadonlyArray<number>;
  }> = [];
  const selected: SelectedEvent[] = [];
  const trace = {
    engine: {
      query: async (sql: string) => {
        queries.push(sql);
        return queryResult(ids);
      },
    },
    selection: {
      resolveSqlEvents: async (table: string, ids: ReadonlyArray<number>) => {
        resolvedArgs.push({table, ids});
        return resolved !== undefined && ids.includes(resolved.eventId)
          ? [resolved]
          : [];
      },
      selectTrackEvent: async (
        trackUri: string,
        eventId: number,
        opts?: SelectionOpts,
      ) => {
        selected.push({trackUri, eventId, opts});
      },
    },
  } as unknown as Trace;
  return {trace, queries, resolvedArgs, selected};
}

// A selected actual frame-timeline slice.
function sliceSelection(eventId: number): TrackEventSelection {
  return {
    kind: 'track_event',
    trackUri: '/process_1/actual_frames',
    eventId,
  } as unknown as TrackEventSelection;
}

describe('VideoFrameLinkSection', () => {
  afterEach(() => {
    document.body.innerHTML = '';
  });

  test('looks up the video frame by the vsync id of the selected slice', async () => {
    const {trace, queries} = fakeTrace([]);
    await new VideoFrameLinkSection(trace).load(sliceSelection(42));

    expect(queries).toHaveLength(1);
    expect(queries[0]).toContain('WHERE s.id = 42');
    expect(queries[0]).toContain(
      'vf.frame_timeline_vsync_id = s.display_frame_token',
    );
    expect(queries[0]).toContain('ORDER BY vf.id');
  });

  test('renders nothing when no video frame matches', async () => {
    const {trace, resolvedArgs} = fakeTrace([], {
      trackUri: '/video_frames/3',
      eventId: 7,
    });
    const section = new VideoFrameLinkSection(trace);
    await section.load(sliceSelection(42));

    expect(resolvedArgs).toEqual([]);
    expect(section.render()).toBeUndefined();
  });

  test('renders nothing when the video frame does not resolve to a track', async () => {
    // E.g. the VideoFrames plugin is disabled, or the frame precedes the
    // display's first key frame and is excluded by the track's dataset.
    const {trace, resolvedArgs} = fakeTrace([7]);
    const section = new VideoFrameLinkSection(trace);
    await section.load(sliceSelection(42));

    expect(resolvedArgs).toEqual([
      {table: '__intrinsic_video_frames', ids: [7]},
    ]);
    expect(section.render()).toBeUndefined();
  });

  test('links to the matching video frame on its display track', async () => {
    const {trace, resolvedArgs, selected} = fakeTrace([7], {
      trackUri: '/video_frames/3',
      eventId: 7,
    });
    const section = new VideoFrameLinkSection(trace);
    await section.load(sliceSelection(42));

    expect(resolvedArgs).toEqual([
      {table: '__intrinsic_video_frames', ids: [7]},
    ]);

    const root = document.createElement('div');
    document.body.appendChild(root);
    m.render(root, section.render());
    expect(root.textContent).toContain('Display video');
    const anchor = root.querySelector<HTMLElement>('a.pf-anchor');
    expect(anchor?.textContent).toContain('Jump to video frame');

    anchor?.click();
    expect(selected).toEqual([
      {
        trackUri: '/video_frames/3',
        eventId: 7,
        opts: {scrollToSelection: true},
      },
    ]);
  });
});
