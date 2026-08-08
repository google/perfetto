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

import {createFakeTraceImpl} from './fake_trace_impl';
import {
  deserializeAppStatePhase2,
  deserializeTrackNode,
  serializeAppState,
  serializeTrackNode,
} from './state_serialization';
import {TrackNode} from '../public/workspace';

describe('state_serialization', () => {
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
    expect(serialized.workspaces[0].title).toBe('Custom Workspace');
    expect(serialized.workspaces[0].userEditable).toBe(false);
    expect(serialized.workspaces[0].tracks).toHaveLength(1);
    expect(serialized.workspaces[0].tracks[0].name).toBe('Custom Track');
    expect(serialized.workspaces[0].tracks[0].uri).toBe('foo.bar.track');
    expect(serialized.workspaces[0].pinnedTracks).toHaveLength(1);
    expect(serialized.workspaces[0].pinnedTracks[0].uri).toBe('foo.bar.pinned');

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
    expect(serialized.currentWorkspace).toBe(customWs.uuid);

    const targetTrace = createFakeTraceImpl();
    deserializeAppStatePhase2(serialized, targetTrace);

    expect(targetTrace.workspaces.currentWorkspace.title).toBe(
      'Active Workspace',
    );
  });

  test('deduplicate workspaces by uuid when loading multiple times', () => {
    const trace = createFakeTraceImpl();

    const customWs = trace.workspaces.createEmptyWorkspace(
      'Plugin Workspace',
      'stable-plugin-uuid-1234',
    );
    customWs.tracks.addChildLast(
      new TrackNode({name: 'Track A', uri: 'track.a'}),
    );

    const serialized = serializeAppState(trace);
    expect(serialized.workspaces[0].uuid).toBe('stable-plugin-uuid-1234');

    const targetTrace = createFakeTraceImpl();
    deserializeAppStatePhase2(serialized, targetTrace);
    deserializeAppStatePhase2(serialized, targetTrace);

    const nonDefaultWorkspaces = targetTrace.workspaces.all.filter(
      (w) => w !== targetTrace.defaultWorkspace,
    );
    expect(nonDefaultWorkspaces).toHaveLength(1);
    expect(nonDefaultWorkspaces[0].uuid).toBe('stable-plugin-uuid-1234');
    expect(nonDefaultWorkspaces[0].title).toBe('Plugin Workspace');
    expect(nonDefaultWorkspaces[0].tracks.children).toHaveLength(1);
  });
});
