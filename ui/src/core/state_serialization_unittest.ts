// Copyright (C) 2024 The Android Open Source Project
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

import {z} from 'zod';
import {Time} from '../base/time';
import type {TrackEventDetailsPanel} from '../public/details_panel';
import type {Track} from '../public/track';
import {createFakeTraceImpl} from './fake_trace_impl';
import {
  deserializeAppStatePhase1,
  deserializeAppStatePhase2,
  JsonSerialize,
  parseAppState,
  serializeAppState,
} from './state_serialization';

vi.hoisted(() => {
  const store = new Map<string, string>();
  const mockStorage = {
    getItem: (k: string) => store.get(k) ?? null,
    setItem: (k: string, v: string) => store.set(k, v),
    removeItem: (k: string) => store.delete(k),
    clear: () => store.clear(),
    key: (i: number) => Array.from(store.keys())[i] ?? null,
    length: 0,
  };
  Object.defineProperty(globalThis, 'localStorage', {
    value: mockStorage,
    configurable: true,
    writable: true,
  });
});

function createMockTrack(uri: string): Track {
  return {
    uri,
    renderer: {
      render: () => {},
      async getSelectionDetails(eventId: number) {
        if (eventId === 123) {
          return {
            ts: Time.fromRaw(1000n),
            dur: 500n,
          };
        }
        return undefined;
      },
    },
  };
}

// Creates a track whose renderer provides a details panel that supports
// permalink serialization. The most recently created panel is exposed via
// getPanel() so tests can inspect/mutate its serialized state.
function createDetailsPanelTrack(uri: string): {
  track: Track;
  getPanel: () => TrackEventDetailsPanel | undefined;
} {
  let latestPanel: TrackEventDetailsPanel | undefined;
  return {
    track: {
      uri,
      renderer: {
        render: () => {},
        async getSelectionDetails(eventId: number) {
          if (eventId === 123) {
            return {
              ts: Time.fromRaw(1000n),
              dur: 500n,
            };
          }
          return undefined;
        },
        detailsPanel: () => {
          const panel: TrackEventDetailsPanel = {
            render: () => null,
            serialization: {
              schema: z.object({tab: z.string()}),
              state: {tab: 'cpu'},
            },
          };
          latestPanel = panel;
          return panel;
        },
      },
    },
    getPanel: () => latestPanel,
  };
}

describe('state serialization', () => {
  test('restores track event selection', async () => {
    const trace1 = createFakeTraceImpl({allowQueries: true});
    trace1.tracks.registerTrack(createMockTrack('test_track'));

    await trace1.selection.selectTrackEvent('test_track', 123);
    expect(trace1.selection.selection).toEqual({
      kind: 'track_event',
      trackUri: 'test_track',
      eventId: 123,
      ts: Time.fromRaw(1000n),
      dur: 500n,
    });

    const serialized = serializeAppState(trace1);
    const json = JsonSerialize(serialized);
    const parsed = JSON.parse(json);
    const parseResult = parseAppState(parsed);
    expect(parseResult.ok).toBe(true);
    if (!parseResult.ok) return;

    const trace2 = createFakeTraceImpl({allowQueries: true});
    trace2.tracks.registerTrack(createMockTrack('test_track'));

    deserializeAppStatePhase1(parseResult.value, trace2);
    deserializeAppStatePhase2(parseResult.value, trace2);

    await vi.waitFor(() => {
      expect(trace2.selection.selection).toEqual({
        kind: 'track_event',
        trackUri: 'test_track',
        eventId: 123,
        ts: Time.fromRaw(1000n),
        dur: 500n,
      });
    });
  });

  test('restores details panel state', async () => {
    const trace1 = createFakeTraceImpl({allowQueries: true});
    const {track, getPanel} = createDetailsPanelTrack('test_track');
    trace1.tracks.registerTrack(track);

    await trace1.selection.selectTrackEvent('test_track', 123);
    // Simulate the user switching the details panel to a non-default tab.
    getPanel()!.serialization!.state = {tab: 'mem'};

    const serialized = serializeAppState(trace1);
    const json = JsonSerialize(serialized);
    const parsed = JSON.parse(json);
    const parseResult = parseAppState(parsed);
    expect(parseResult.ok).toBe(true);
    if (!parseResult.ok) return;
    // The details panel state must be captured in the serialized selection.
    expect(parseResult.value.selection[0]).toMatchObject({
      kind: 'TRACK_EVENT',
      detailsPanel: {tab: 'mem'},
    });

    const trace2 = createFakeTraceImpl({allowQueries: true});
    trace2.tracks.registerTrack(createDetailsPanelTrack('test_track').track);

    deserializeAppStatePhase1(parseResult.value, trace2);
    deserializeAppStatePhase2(parseResult.value, trace2);

    await vi.waitFor(() => {
      expect(
        trace2.selection.getDetailsPanelForSelection()?.serializatonState(),
      ).toEqual({tab: 'mem'});
    });
  });

  test('survives when the details panel is not present on restore', async () => {
    const trace1 = createFakeTraceImpl({allowQueries: true});
    const {track, getPanel} = createDetailsPanelTrack('test_track');
    trace1.tracks.registerTrack(track);

    await trace1.selection.selectTrackEvent('test_track', 123);
    getPanel()!.serialization!.state = {tab: 'mem'};

    const serialized = serializeAppState(trace1);
    const json = JsonSerialize(serialized);
    const parsed = JSON.parse(json);
    const parseResult = parseAppState(parsed);
    expect(parseResult.ok).toBe(true);
    if (!parseResult.ok) return;
    expect(parseResult.value.selection[0]).toMatchObject({
      kind: 'TRACK_EVENT',
      detailsPanel: {tab: 'mem'},
    });

    // The receiving trace has a track WITHOUT a details panel.
    const trace2 = createFakeTraceImpl({allowQueries: true});
    trace2.tracks.registerTrack(createMockTrack('test_track'));

    deserializeAppStatePhase1(parseResult.value, trace2);
    deserializeAppStatePhase2(parseResult.value, trace2);

    // The selection is still restored; there is simply no details panel.
    await vi.waitFor(() => {
      expect(trace2.selection.selection).toEqual({
        kind: 'track_event',
        trackUri: 'test_track',
        eventId: 123,
        ts: Time.fromRaw(1000n),
        dur: 500n,
      });
    });
    expect(trace2.selection.getDetailsPanelForSelection()).toBeUndefined();
  });
});
