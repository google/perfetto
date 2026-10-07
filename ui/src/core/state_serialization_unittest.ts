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
import {TrackNode} from '../public/workspace';
import {createFakeTraceImpl} from './fake_trace_impl';
import {
  deserializeAppStatePhase1,
  deserializeAppStatePhase2,
  deserializeTrackNode,
  JsonSerialize,
  parseAppState,
  serializeAppState,
  serializeTrackNode,
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

  test('serialize and deserialize TrackNode', () => {
    const root = new TrackNode({
      name: 'Group A',
      collapsed: false,
      isSummary: true,
    });
    const child = new TrackNode({
      name: 'Track 1',
      uri: 'test.track.1',
      collapsed: true,
      removable: true,
    });
    root.addChildLast(child);

    const serialized = serializeTrackNode(root);
    expect(serialized.name).toBe('Group A');
    expect(serialized.collapsed).toBe(false);
    expect(serialized.isSummary).toBe(true);
    expect(serialized.children).toHaveLength(1);
    expect(serialized.children![0].name).toBe('Track 1');
    expect(serialized.children![0].uri).toBe('test.track.1');

    const deserialized = deserializeTrackNode(serialized);
    expect(deserialized.name).toBe('Group A');
    expect(deserialized.collapsed).toBe(false);
    expect(deserialized.isSummary).toBe(true);
    expect(deserialized.children).toHaveLength(1);
    expect(deserialized.children[0].name).toBe('Track 1');
    expect(deserialized.children[0].uri).toBe('test.track.1');
    expect(deserialized.children[0].removable).toBe(true);
  });

  test('serialize and deserialize non-default workspaces', () => {
    const trace = createFakeTraceImpl();

    const customWs = trace.workspaces.createEmptyWorkspace('Custom Workspace');
    customWs.userEditable = false;

    const track = new TrackNode({
      name: 'Custom Track',
      uri: 'foo.bar.track',
    });
    customWs.tracks.addChildLast(track);

    const pinnedTrack = new TrackNode({
      name: 'Pinned Track',
      uri: 'foo.bar.pinned',
    });
    customWs.pinnedTracksNode.addChildLast(pinnedTrack);

    const serialized = serializeAppState(trace);
    expect(serialized.workspaces).toHaveLength(1);
    expect(serialized.workspaces![0].title).toBe('Custom Workspace');
    expect(serialized.workspaces![0].userEditable).toBe(false);
    expect(serialized.workspaces![0].tracks).toHaveLength(1);
    expect(serialized.workspaces![0].tracks[0].name).toBe('Custom Track');
    expect(serialized.workspaces![0].tracks[0].uri).toBe('foo.bar.track');
    expect(serialized.workspaces![0].pinnedTracks).toHaveLength(1);
    expect(serialized.workspaces![0].pinnedTracks[0].uri).toBe(
      'foo.bar.pinned',
    );

    const targetTrace = createFakeTraceImpl();
    deserializeAppStatePhase2(serialized, targetTrace);

    const workspaces = targetTrace.workspaces.all;
    const restoredWs = workspaces.find((w) => w.title === 'Custom Workspace');
    expect(restoredWs).toBeDefined();
    expect(restoredWs!.userEditable).toBe(false);
    expect(restoredWs!.tracks.children).toHaveLength(1);
    expect(restoredWs!.tracks.children[0].name).toBe('Custom Track');
    expect(restoredWs!.tracks.children[0].uri).toBe('foo.bar.track');
    expect(restoredWs!.pinnedTracks).toHaveLength(1);
    expect(restoredWs!.pinnedTracks[0].name).toBe('Pinned Track');
    expect(restoredWs!.pinnedTracks[0].uri).toBe('foo.bar.pinned');
  });

  test('serialize and deserialize current workspace', () => {
    const trace = createFakeTraceImpl();

    const customWs = trace.workspaces.createEmptyWorkspace('Active Workspace');
    trace.workspaces.switchWorkspace(customWs);

    const serialized = serializeAppState(trace);
    expect(serialized.currentWorkspace).toBe(customWs.id);

    const targetTrace = createFakeTraceImpl();
    deserializeAppStatePhase2(serialized, targetTrace);

    expect(targetTrace.workspaces.currentWorkspace.title).toBe(
      'Active Workspace',
    );
    expect(targetTrace.workspaces.currentWorkspace.id).toBe(customWs.id);
  });

  test('serialize and deserialize default workspace as current workspace', () => {
    const trace = createFakeTraceImpl();

    const serialized = serializeAppState(trace);
    expect(serialized.currentWorkspace).toBe(trace.defaultWorkspace.id);

    const targetTrace = createFakeTraceImpl();
    const otherWs =
      targetTrace.workspaces.createEmptyWorkspace('Other Workspace');
    targetTrace.workspaces.switchWorkspace(otherWs);

    deserializeAppStatePhase2(serialized, targetTrace);

    expect(targetTrace.workspaces.currentWorkspace).toBe(
      targetTrace.defaultWorkspace,
    );
    expect(targetTrace.workspaces.currentWorkspace.id).toBe(
      serialized.currentWorkspace,
    );
  });

  test('replaces non-default workspaces when loading multiple times', () => {
    const trace = createFakeTraceImpl();

    const customWs = trace.workspaces.createEmptyWorkspace(
      'Plugin Workspace',
      'stable-plugin-id-1234',
    );
    customWs.tracks.addChildLast(
      new TrackNode({name: 'Track A', uri: 'track.a'}),
    );

    const serialized = serializeAppState(trace);
    expect(serialized.workspaces![0].id).toBe('stable-plugin-id-1234');

    const targetTrace = createFakeTraceImpl();
    deserializeAppStatePhase2(serialized, targetTrace);
    deserializeAppStatePhase2(serialized, targetTrace);

    const nonDefaultWorkspaces = targetTrace.workspaces.all.filter(
      (w) => w !== targetTrace.defaultWorkspace,
    );
    expect(nonDefaultWorkspaces).toHaveLength(1);
    expect(nonDefaultWorkspaces[0].id).toBe('stable-plugin-id-1234');
    expect(nonDefaultWorkspaces[0].title).toBe('Plugin Workspace');
    expect(nonDefaultWorkspaces[0].tracks.children).toHaveLength(1);
  });

  test('legacy state without workspaces preserves existing non-default workspaces', () => {
    const targetTrace = createFakeTraceImpl();
    targetTrace.workspaces.createEmptyWorkspace('Plugin Workspace');

    const legacyState = parseAppState({version: 1});
    expect(legacyState.ok).toBe(true);
    if (!legacyState.ok) return;
    expect(legacyState.value.workspaces).toBeUndefined();

    deserializeAppStatePhase2(legacyState.value, targetTrace);

    const nonDefaultWorkspaces = targetTrace.workspaces.all.filter(
      (w) => w !== targetTrace.defaultWorkspace,
    );
    expect(nonDefaultWorkspaces).toHaveLength(1);
    expect(nonDefaultWorkspaces[0].title).toBe('Plugin Workspace');
  });

  test('state with empty workspaces removes existing non-default workspaces', () => {
    const targetTrace = createFakeTraceImpl();
    targetTrace.workspaces.createEmptyWorkspace('Plugin Workspace');

    const state = parseAppState({version: 1, workspaces: []});
    expect(state.ok).toBe(true);
    if (!state.ok) return;
    expect(state.value.workspaces).toEqual([]);

    deserializeAppStatePhase2(state.value, targetTrace);

    const nonDefaultWorkspaces = targetTrace.workspaces.all.filter(
      (w) => w !== targetTrace.defaultWorkspace,
    );
    expect(nonDefaultWorkspaces).toHaveLength(0);
  });
});
