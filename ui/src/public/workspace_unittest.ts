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
    const track = new TrackNode();
    workspace.addChildLast(track);

    expect(workspace.getTrackById(track.id)).toEqual(track);
  });

  test('getNodeByKey', () => {
    const track = new TrackNode();

    const group = new TrackNode();
    group.addChildLast(track);

    // Add group to workspace AFTER adding the track to the group
    const workspace = new Workspace();
    workspace.addChildLast(group);

    expect(workspace.getTrackById(track.id)).toBe(track);
  });

  test('nested index lookup', () => {
    const track = new TrackNode();

    const group = new TrackNode();

    // Add group to workspace before adding the track to the group
    const workspace = new Workspace();
    workspace.addChildLast(group);
    group.addChildLast(track);

    expect(workspace.getTrackById(track.id)).toBe(track);
  });

  test('nested index lookup after remove', () => {
    const workspace = new Workspace();

    const group = new TrackNode();

    const track = new TrackNode();
    group.addChildLast(track);

    // Add group to workspace
    workspace.addChildLast(group);
    workspace.removeChild(group);

    expect(workspace.getTrackById(track.id)).toBe(undefined);
  });

  test('getTrackByUri()', () => {
    const workspace = new Workspace();

    const group = new TrackNode();

    const track = new TrackNode({uri: 'foo'});
    group.addChildLast(track);

    // Add group to workspace
    workspace.addChildLast(group);

    expect(workspace.getTrackByUri('foo')).toBe(track);
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
    const child = new TrackNode();

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

test('TrackNode::flatTracksOrdered', () => {
  const root = new TrackNode();

  const removeme = new TrackNode({uri: 'removeme'});
  root.addChildFirst(removeme);

  const foo = new TrackNode({uri: 'foo'});
  root.addChildLast(foo);
  foo.addChildLast(new TrackNode({uri: 'fooBar'})); // <-- Note this one is added as a child of foo
  const bar = new TrackNode({uri: 'bar'});
  root.addChildLast(bar);
  root.addChildFirst(new TrackNode({uri: 'baz'})); // <- Note this one is added first so should appear before the others in flatTracks

  root.removeChild(removeme);

  expect(root.flatTracksOrdered.map(({uri}) => uri)).toEqual([
    'baz',
    'foo',
    'fooBar',
    'bar',
  ]);
});

test('TrackNode::flatTracks', () => {
  const root = new TrackNode();

  const removeme = new TrackNode({uri: 'removeme'});
  root.addChildFirst(removeme);

  const foo = new TrackNode({uri: 'foo'});
  root.addChildLast(foo);
  foo.addChildLast(new TrackNode({uri: 'fooBar'})); // <-- Note this one is added as a child of foo
  root.addChildLast(new TrackNode({uri: 'bar'}));
  root.addChildFirst(new TrackNode({uri: 'baz'})); // <- Note this one is added first so should appear before the others in flatTracks

  root.removeChild(removeme);

  expect(root.flatTracks.map(({uri}) => uri)).toEqual(
    expect.arrayContaining(['baz', 'foo', 'fooBar', 'bar']),
  );
  expect(root.flatTracks.length).toBe(4);
});

test('TrackNode::clone', () => {
  const root = new TrackNode();
  const childA = new TrackNode();
  root.addChildLast(childA);

  const childB = new TrackNode();
  root.addChildLast(childB);

  const cloned = root.clone();

  expect(cloned.id).not.toBe(root.id); // id should be different
  expect(cloned.uri).toBe(root.uri);
  expect(cloned.expanded).toBe(root.expanded);
  expect(cloned.name).toBe(root.name);
  expect(cloned.headless).toBe(root.headless);
  expect(cloned.isSummary).toBe(root.isSummary);
  expect(cloned.removable).toBe(root.removable);
  expect(cloned.children).toStrictEqual([]); // Children should not be copied
});

test('TrackNode::clone(deep)', () => {
  const root = new TrackNode();
  const childA = new TrackNode();
  root.addChildLast(childA);

  const childB = new TrackNode();
  root.addChildLast(childB);

  const cloned = root.clone(true);

  expect(cloned.id).not.toBe(root.id); // id should be different
  expect(cloned.uri).toBe(root.uri);
  expect(cloned.expanded).toBe(root.expanded);
  expect(cloned.name).toBe(root.name);
  expect(cloned.headless).toBe(root.headless);
  expect(cloned.isSummary).toBe(root.isSummary);
  expect(cloned.removable).toBe(root.removable);
  expect(cloned.children).toHaveLength(2);

  expect(cloned.children[0].name).toBe(childA.name);
  expect(cloned.children[0].uri).toBe(childA.uri);

  expect(cloned.children[1].name).toBe(childB.name);
  expect(cloned.children[1].uri).toBe(childB.uri);
});
