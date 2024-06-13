// Copyright (C) 2018 The Android Open Source Project
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

import {produce} from 'immer';

import {assertExists} from '../base/logging';
import {PrimaryTrackSortKey} from '../public';
import {HEAP_PROFILE_TRACK_KIND} from '../core_plugins/heap_profile';
import {PROCESS_SCHEDULING_TRACK_KIND} from '../core_plugins/process_summary/process_scheduling_track';

import {StateActions} from './actions';
import {createEmptyState} from './empty_state';
import {
  InThreadTrackSortKey,
  SCROLLING_TRACK_GROUP,
  State,
  TraceUrlSource,
  TrackSortKey,
} from './state';
import {
  THREAD_SLICE_TRACK_KIND,
  THREAD_STATE_TRACK_KIND,
} from '../core/track_kinds';

function fakeTrack(
  state: State,
  args: {
    key: string;
    uri?: string;
    trackGroup?: string;
    trackSortKey?: TrackSortKey;
    name?: string;
    tid?: string;
  },
): State {
  return produce(state, (draft) => {
    StateActions.addTrack(draft, {
      uri: args.uri || 'sometrack',
      key: args.key,
      name: args.name || 'A track',
      trackSortKey:
        args.trackSortKey === undefined
          ? PrimaryTrackSortKey.ORDINARY_TRACK
          : args.trackSortKey,
      trackGroup: args.trackGroup || SCROLLING_TRACK_GROUP,
    });
  });
}

function fakeTrackGroup(
  state: State,
  args: {key: string; summaryTrackKey: string},
): State {
  return produce(state, (draft) => {
    StateActions.addTrackGroup(draft, {
      name: 'A group',
      key: args.key,
      collapsed: false,
      summaryTrackKey: args.summaryTrackKey,
    });
  });
}

function pinnedAndScrollingTracks(
  state: State,
  keys: string[],
  pinnedTracks: string[],
  scrollingTracks: string[],
): State {
  for (const key of keys) {
    state = fakeTrack(state, {key});
  }
  state = produce(state, (draft) => {
    draft.pinnedTracks = pinnedTracks;
    draft.scrollingTracks = scrollingTracks;
  });
  return state;
}

test('add scrolling tracks', () => {
  const once = produce(createEmptyState(), (draft) => {
    StateActions.addTrack(draft, {
      uri: 'cpu',
      name: 'Cpu 1',
      trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
    });
  });
  const twice = produce(once, (draft) => {
    StateActions.addTrack(draft, {
      uri: 'cpu',
      name: 'Cpu 2',
      trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
      trackGroup: SCROLLING_TRACK_GROUP,
    });
  });

  expect(Object.values(twice.tracks).length).toBe(2);
  expect(twice.scrollingTracks.length).toBe(2);
});

test('add track to track group', () => {
  let state = createEmptyState();
  state = fakeTrack(state, {key: 's'});

  const afterGroup = produce(state, (draft) => {
    StateActions.addTrackGroup(draft, {
      name: 'A track group',
      key: '123-123-123',
      summaryTrackKey: 's',
      collapsed: false,
    });
  });

  const afterTrackAdd = produce(afterGroup, (draft) => {
    StateActions.addTrack(draft, {
      key: '1',
      uri: 'slices',
      name: 'renderer 1',
      trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
      trackGroup: '123-123-123',
    });
  });

  expect(afterTrackAdd.trackGroups['123-123-123'].summaryTrack).toBe('s');
  expect(afterTrackAdd.trackGroups['123-123-123'].tracks[0]).toBe('1');
});

test('reorder tracks', () => {
  const once = produce(createEmptyState(), (draft) => {
    StateActions.addTrack(draft, {
      uri: 'cpu',
      name: 'Cpu 1',
      trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
    });
    StateActions.addTrack(draft, {
      uri: 'cpu',
      name: 'Cpu 2',
      trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
    });
  });

  const firstTrackKey = once.scrollingTracks[0];
  const secondTrackKey = once.scrollingTracks[1];

  const twice = produce(once, (draft) => {
    StateActions.moveTrack(draft, {
      srcId: `${firstTrackKey}`,
      op: 'after',
      dstId: `${secondTrackKey}`,
    });
  });

  expect(twice.scrollingTracks[0]).toBe(secondTrackKey);
  expect(twice.scrollingTracks[1]).toBe(firstTrackKey);
});

test('reorder pinned to scrolling', () => {
  let state = createEmptyState();
  state = pinnedAndScrollingTracks(state, ['a', 'b', 'c'], ['a', 'b'], ['c']);

  const after = produce(state, (draft) => {
    StateActions.moveTrack(draft, {
      srcId: 'b',
      op: 'before',
      dstId: 'c',
    });
  });

  expect(after.pinnedTracks).toEqual(['a']);
  expect(after.scrollingTracks).toEqual(['b', 'c']);
});

test('reorder scrolling to pinned', () => {
  let state = createEmptyState();
  state = pinnedAndScrollingTracks(state, ['a', 'b', 'c'], ['a'], ['b', 'c']);

  const after = produce(state, (draft) => {
    StateActions.moveTrack(draft, {
      srcId: 'b',
      op: 'after',
      dstId: 'a',
    });
  });

  expect(after.pinnedTracks).toEqual(['a', 'b']);
  expect(after.scrollingTracks).toEqual(['c']);
});

test('reorder clamp bottom', () => {
  let state = createEmptyState();
  state = pinnedAndScrollingTracks(state, ['a', 'b', 'c'], ['a', 'b'], ['c']);

  const after = produce(state, (draft) => {
    StateActions.moveTrack(draft, {
      srcId: 'a',
      op: 'before',
      dstId: 'a',
    });
  });
  expect(after).toEqual(state);
});

test('reorder clamp top', () => {
  let state = createEmptyState();
  state = pinnedAndScrollingTracks(state, ['a', 'b', 'c'], ['a'], ['b', 'c']);

  const after = produce(state, (draft) => {
    StateActions.moveTrack(draft, {
      srcId: 'c',
      op: 'after',
      dstId: 'c',
    });
  });
  expect(after).toEqual(state);
});

test('pin', () => {
  let state = createEmptyState();
  state = pinnedAndScrollingTracks(state, ['a', 'b', 'c'], ['a'], ['b', 'c']);

  const after = produce(state, (draft) => {
    StateActions.toggleTrackPinned(draft, {
      trackKey: 'c',
    });
  });
  expect(after.pinnedTracks).toEqual(['a', 'c']);
  expect(after.scrollingTracks).toEqual(['b']);
});

test('unpin', () => {
  let state = createEmptyState();
  state = pinnedAndScrollingTracks(state, ['a', 'b', 'c'], ['a', 'b'], ['c']);

  const after = produce(state, (draft) => {
    StateActions.toggleTrackPinned(draft, {
      trackKey: 'a',
    });
  });
  expect(after.pinnedTracks).toEqual(['b']);
  expect(after.scrollingTracks).toEqual(['a', 'c']);
});

test('open trace', () => {
  const state = createEmptyState();
  const recordConfig = state.recordConfig;
  const after = produce(state, (draft) => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/bar',
    });
  });

  expect(after.engine).not.toBeUndefined();
  expect((after.engine!!.source as TraceUrlSource).url).toBe(
    'https://example.com/bar',
  );
  expect(after.recordConfig).toBe(recordConfig);
});

test('open second trace from file', () => {
  const once = produce(createEmptyState(), (draft) => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/bar',
    });
  });

  const twice = produce(once, (draft) => {
    StateActions.addTrack(draft, {
      uri: 'cpu',
      name: 'Cpu 1',
      trackSortKey: PrimaryTrackSortKey.ORDINARY_TRACK,
    });
  });

  const thrice = produce(twice, (draft) => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/foo',
    });
  });

  expect(thrice.engine).not.toBeUndefined();
  expect((thrice.engine!!.source as TraceUrlSource).url).toBe(
    'https://example.com/foo',
  );
  expect(thrice.pinnedTracks.length).toBe(0);
  expect(thrice.scrollingTracks.length).toBe(0);
});

test('setEngineReady with missing engine is ignored', () => {
  const state = createEmptyState();
  produce(state, (draft) => {
    StateActions.setEngineReady(draft, {
      engineId: '1',
      ready: true,
      mode: 'WASM',
    });
  });
});

test('setEngineReady', () => {
  const state = createEmptyState();
  const after = produce(state, (draft) => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/bar',
    });
    const latestEngineId = assertExists(draft.engine).id;
    StateActions.setEngineReady(draft, {
      engineId: latestEngineId,
      ready: true,
      mode: 'WASM',
    });
  });
  expect(after.engine!!.ready).toBe(true);
});

test('sortTracksByPriority', () => {
  let state = createEmptyState();
  state = fakeTrackGroup(state, {key: 'g', summaryTrackKey: 'b'});
  state = fakeTrack(state, {
    key: 'b',
    uri: HEAP_PROFILE_TRACK_KIND,
    trackSortKey: PrimaryTrackSortKey.HEAP_PROFILE_TRACK,
    trackGroup: 'g',
  });
  state = fakeTrack(state, {
    key: 'a',
    uri: PROCESS_SCHEDULING_TRACK_KIND,
    trackSortKey: PrimaryTrackSortKey.PROCESS_SCHEDULING_TRACK,
    trackGroup: 'g',
  });

  const after = produce(state, (draft) => {
    StateActions.sortThreadTracks(draft, {});
  });

  // High Priority tracks should be sorted before Low Priority tracks:
  // 'b' appears twice because it's the summary track
  expect(after.trackGroups['g'].tracks).toEqual(['a', 'b']);
});

test('sortTracksByPriorityAndKindAndName', () => {
  let state = createEmptyState();
  state = fakeTrackGroup(state, {key: 'g', summaryTrackKey: 'b'});
  state = fakeTrack(state, {
    key: 'a',
    uri: PROCESS_SCHEDULING_TRACK_KIND,
    trackSortKey: PrimaryTrackSortKey.PROCESS_SCHEDULING_TRACK,
    trackGroup: 'g',
  });
  state = fakeTrack(state, {
    key: 'b',
    uri: THREAD_SLICE_TRACK_KIND,
    trackGroup: 'g',
    trackSortKey: PrimaryTrackSortKey.MAIN_THREAD,
  });
  state = fakeTrack(state, {
    key: 'c',
    uri: THREAD_SLICE_TRACK_KIND,
    trackGroup: 'g',
    trackSortKey: PrimaryTrackSortKey.RENDER_THREAD,
  });
  state = fakeTrack(state, {
    key: 'd',
    uri: THREAD_SLICE_TRACK_KIND,
    trackGroup: 'g',
    trackSortKey: PrimaryTrackSortKey.GPU_COMPLETION_THREAD,
  });
  state = fakeTrack(state, {
    key: 'e',
    uri: HEAP_PROFILE_TRACK_KIND,
    trackGroup: 'g',
  });
  state = fakeTrack(state, {
    key: 'f',
    uri: THREAD_SLICE_TRACK_KIND,
    trackGroup: 'g',
    name: 'T2',
  });
  state = fakeTrack(state, {
    key: 'g',
    uri: THREAD_SLICE_TRACK_KIND,
    trackGroup: 'g',
    name: 'T10',
  });

  const after = produce(state, (draft) => {
    StateActions.sortThreadTracks(draft, {});
  });

  // The order should be determined by:
  // 1.High priority
  // 2.Non ordinary track kinds
  // 3.Low priority
  // 4.Collated name string (ie. 'T2' will be before 'T10')
  expect(after.trackGroups['g'].tracks).toEqual([
    'a',
    'b',
    'c',
    'd',
    'e',
    'f',
    'g',
  ]);
});

test('sortTracksByTidThenName', () => {
  let state = createEmptyState();
  state = fakeTrackGroup(state, {key: 'g', summaryTrackKey: 'a'});
  state = fakeTrack(state, {
    key: 'a',
    uri: THREAD_SLICE_TRACK_KIND,
    trackSortKey: {
      utid: 1,
      priority: InThreadTrackSortKey.ORDINARY,
    },
    trackGroup: 'g',
    name: 'aaa',
    tid: '1',
  });
  state = fakeTrack(state, {
    key: 'b',
    uri: THREAD_SLICE_TRACK_KIND,
    trackSortKey: {
      utid: 2,
      priority: InThreadTrackSortKey.ORDINARY,
    },
    trackGroup: 'g',
    name: 'bbb',
    tid: '2',
  });
  state = fakeTrack(state, {
    key: 'c',
    uri: THREAD_STATE_TRACK_KIND,
    trackSortKey: {
      utid: 1,
      priority: InThreadTrackSortKey.ORDINARY,
    },
    trackGroup: 'g',
    name: 'ccc',
    tid: '1',
  });

  const after = produce(state, (draft) => {
    StateActions.sortThreadTracks(draft, {});
  });

  expect(after.trackGroups['g'].tracks).toEqual(['a', 'c', 'b']);
});
