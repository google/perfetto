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

interface VideoFrameRow {
  readonly id: number;
  readonly displayId: number;
}

// Builds the (id, displayId) query result the section expects, with every cell
// encoded as a varint.
function queryResult(rows: ReadonlyArray<VideoFrameRow>): QueryResult {
  const batch = protos.QueryResult.CellsBatch.create({
    cells: rows.flatMap(() => [CELL_TYPE.CELL_VARINT, CELL_TYPE.CELL_VARINT]),
    varintCells: rows.flatMap((row) => [row.id, row.displayId]),
    isLastBatch: true,
  });
  const result = createQueryResult({query: 'fake'});
  result.appendResultBatch(
    protos.QueryResult.encode(
      protos.QueryResult.create({
        columnNames: ['id', 'displayId'],
        batch: [batch],
      }),
    ).finish(),
  );
  return result;
}

interface SelectedEvent {
  readonly trackUri: string;
  readonly eventId: number;
  readonly opts?: SelectionOpts;
}

// A trace whose engine answers any query with `rows`, recording the queries it
// runs and the track events it selects. Only the tracks in `trackUris` are
// registered.
function fakeTrace(
  rows: ReadonlyArray<VideoFrameRow>,
  trackUris: ReadonlyArray<string> = [],
) {
  const queries: string[] = [];
  const selected: SelectedEvent[] = [];
  const trace = {
    engine: {
      query: async (sql: string) => {
        queries.push(sql);
        return queryResult(rows);
      },
    },
    tracks: {
      getTrack: (uri: string) => (trackUris.includes(uri) ? {uri} : undefined),
    },
    selection: {
      selectTrackEvent: async (
        trackUri: string,
        eventId: number,
        opts?: SelectionOpts,
      ) => {
        selected.push({trackUri, eventId, opts});
      },
    },
  } as unknown as Trace;
  return {trace, queries, selected};
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
    // Only frames the video track shows: no codec_config rows, and nothing
    // before the display's first key frame.
    expect(queries[0]).toContain('COALESCE(vf.is_config, 0) = 0');
    expect(queries[0]).toContain(
      'WHERE display_id = vf.display_id AND is_key_frame = 1',
    );
    expect(queries[0]).toContain('ORDER BY vf.id');
  });

  test('renders nothing when no video frame matches', async () => {
    const {trace} = fakeTrace([], ['/video_frames/3']);
    const section = new VideoFrameLinkSection(trace);
    await section.load(sliceSelection(42));

    expect(section.render()).toBeUndefined();
  });

  test('renders nothing when the video track is not registered', async () => {
    // E.g. the VideoFrames plugin is disabled.
    const {trace} = fakeTrace([{id: 7, displayId: 3}]);
    const section = new VideoFrameLinkSection(trace);
    await section.load(sliceSelection(42));

    expect(section.render()).toBeUndefined();
  });

  test('links to the matching video frame on its display track', async () => {
    const {trace, selected} = fakeTrace(
      [{id: 7, displayId: 3}],
      ['/video_frames/3'],
    );
    const section = new VideoFrameLinkSection(trace);
    await section.load(sliceSelection(42));

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
