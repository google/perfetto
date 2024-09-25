// Copyright (C) 2023 The Android Open Source Project
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

import {TrackNode, Workspace} from './workspace';

describe('workspace', () => {
  test('getNodeByKey', () => {
    const workspace = new Workspace();
    const track = new TrackNode({id: 'foo'});
    workspace.addChildLast(track);

    expect(workspace.getTrackById('foo')).toEqual(track);
  });

  test('getNodeByKey', () => {
    const track = new TrackNode({id: 'bar'});

    const group = new TrackNode();
    group.addChildLast(track);

    // Add group to workspace AFTER adding the track to the group
    const workspace = new Workspace();
    workspace.addChildLast(group);

    expect(workspace.getTrackById('bar')).toBe(track);
  });

  test('nested index lookup', () => {
    const track = new TrackNode({id: 'bar'});

    const group = new TrackNode();

    // Add group to workspace before adding the track to the group
    const workspace = new Workspace();
    workspace.addChildLast(group);
    group.addChildLast(track);

    expect(workspace.getTrackById('bar')).toBe(track);
  });

  test('nested index lookup', () => {
    const workspace = new Workspace();

    const group = new TrackNode();

    const track = new TrackNode({id: 'bar'});
    group.addChildLast(track);

    // Add group to workspace
    workspace.addChildLast(group);
    workspace.removeChild(group);

    expect(workspace.getTrackById('bar')).toBe(undefined);
  });

  test('findTrackByUri()', () => {
    const workspace = new Workspace();

    const group = new TrackNode();

    const track = new TrackNode({uri: 'foo'});
    group.addChildLast(track);

    // Add group to workspace
    workspace.addChildLast(group);

    expect(workspace.findTrackByUri('foo')).toBe(track);
  });

  test('findClosestVisibleAncestor()', () => {
    const child = new TrackNode();
    child.expand(); // Expanding the child node should have no effect

    const parent = new TrackNode();
    parent.expand();
    parent.addChildLast(child);

    // While everything is expanded and the child node is visible, the child
    // should be returned.
    expect(child.findClosestVisibleAncestor()).toBe(child);

    // Collapse the parent node and this parent should be returned, as from the
    // point of view of the root, this is the closest we can get to our target
    // node without expanding any more nodes.
    parent.collapse();
    expect(child.findClosestVisibleAncestor()).toBe(parent);
  });
});

describe('TrackNode.addChildInOrder', () => {
  let container: TrackNode;

  beforeEach(() => {
    container = new TrackNode();
  });

  test('inserts a child into an empty container', () => {
    const child = new TrackNode({id: 'track1'});

    container.addChildInOrder(child);

    expect(container.children).toHaveLength(1);
    expect(container.children[0]).toBe(child);
  });

  test('inserts a child with a lower sortOrder before an existing child', () => {
    const child1 = new TrackNode({sortOrder: 10});
    const child2 = new TrackNode({sortOrder: 5});

    container.addChildInOrder(child1);
    container.addChildInOrder(child2);

    expect(container.children).toHaveLength(2);
    expect(container.children[0]).toBe(child2);
    expect(container.children[1]).toBe(child1);
  });

  test('inserts a child with a higher sortOrder after an existing child', () => {
    const child1 = new TrackNode({sortOrder: 5});
    const child2 = new TrackNode({sortOrder: 10});

    container.addChildInOrder(child1);
    container.addChildInOrder(child2);

    expect(container.children).toHaveLength(2);
    expect(container.children[0]).toBe(child1);
    expect(container.children[1]).toBe(child2);
  });

  test('inserts a child with the same sortOrder after an existing child', () => {
    const child1 = new TrackNode({sortOrder: 5});
    const child2 = new TrackNode({sortOrder: 5});

    container.addChildInOrder(child1);
    container.addChildInOrder(child2);

    expect(container.children).toHaveLength(2);
    expect(container.children[0]).toBe(child1);
    expect(container.children[1]).toBe(child2);
  });

  test('inserts multiple children and maintains order', () => {
    const child1 = new TrackNode({sortOrder: 15});
    const child2 = new TrackNode({sortOrder: 10});
    const child3 = new TrackNode({sortOrder: 20});

    container.addChildInOrder(child1);
    container.addChildInOrder(child2);
    container.addChildInOrder(child3);

    expect(container.children).toHaveLength(3);
    expect(container.children[0]).toBe(child2);
    expect(container.children[1]).toBe(child1);
    expect(container.children[2]).toBe(child3);
  });

  test('inserts a child with undefined sortOrder as 0', () => {
    const child1 = new TrackNode({sortOrder: 10});

    // sortOrder is undefined, treated as 0
    const child2 = new TrackNode();

    container.addChildInOrder(child1);
    container.addChildInOrder(child2);

    expect(container.children).toHaveLength(2);

    // child2 (sortOrder 0) should be first
    expect(container.children[0]).toBe(child2);
    expect(container.children[1]).toBe(child1);
  });
});
