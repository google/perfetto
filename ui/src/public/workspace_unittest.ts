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

import {GroupNode, TrackNode, Workspace} from './workspace';

test('Workspace', () => {
  const fooTrack = new TrackNode('foo', 'Foo');

  const fooGroup = new GroupNode('foo');
  fooGroup.insertChildInOrder(fooTrack);

  const workspace = new Workspace('Default Workspace');
  workspace.insertChildInOrder(fooGroup);
});

describe('workspace', () => {
  test('index', () => {
    const workspace = new Workspace('My workspace');
    const track = new TrackNode('foo', 'Foo');
    workspace.appendChild(track);

    expect(workspace.getNodeByUri('foo')).toEqual(track);
  });

  test('getNodeByUri', () => {
    const track = new TrackNode('bar', 'Bar');

    const group = new GroupNode('Foo');
    group.appendChild(track);

    // Add group to workspace AFTER adding the track to the group
    const workspace = new Workspace('My workspace');
    workspace.appendChild(group);

    expect(workspace.getNodeByUri('bar')).toBe(track);
  });

  test('nested index lookup', () => {
    const track = new TrackNode('bar', 'Bar');

    const group = new GroupNode('Foo');

    // Add group to workspace before adding the track to the group
    const workspace = new Workspace('My workspace');
    workspace.appendChild(group);
    group.appendChild(track);

    expect(workspace.getNodeByUri('bar')).toBe(track);
  });

  test('nested index lookup', () => {
    const workspace = new Workspace('My workspace');

    const group = new GroupNode('Foo');

    const track = new TrackNode('bar', 'Bar');
    group.appendChild(track);

    // Add group to workspace
    workspace.appendChild(group);
    workspace.removeChild(group);

    expect(workspace.getNodeByUri('bar')).toBe(undefined);
  });
});

describe('GroupNode.insertChildInOrder', () => {
  let container: GroupNode;

  beforeEach(() => {
    container = new GroupNode('Test Container');
  });

  test('inserts a child into an empty container', () => {
    const child = new TrackNode('uri1', 'Track 1');

    container.insertChildInOrder(child);

    expect(container.children).toHaveLength(1);
    expect(container.children[0]).toBe(child);
  });

  test('inserts a child with a lower sortOrder before an existing child', () => {
    const child1 = new TrackNode('uri1', 'Track 1');
    child1.sortOrder = 10;
    const child2 = new TrackNode('uri2', 'Track 2');
    child2.sortOrder = 5;

    container.insertChildInOrder(child1);
    container.insertChildInOrder(child2);

    expect(container.children).toHaveLength(2);
    expect(container.children[0]).toBe(child2);
    expect(container.children[1]).toBe(child1);
  });

  test('inserts a child with a higher sortOrder after an existing child', () => {
    const child1 = new TrackNode('uri1', 'Track 1');
    child1.sortOrder = 5;
    const child2 = new TrackNode('uri2', 'Track 2');
    child2.sortOrder = 10;

    container.insertChildInOrder(child1);
    container.insertChildInOrder(child2);

    expect(container.children).toHaveLength(2);
    expect(container.children[0]).toBe(child1);
    expect(container.children[1]).toBe(child2);
  });

  test('inserts a child with the same sortOrder after an existing child', () => {
    const child1 = new TrackNode('uri1', 'Track 1');
    child1.sortOrder = 5;
    const child2 = new TrackNode('uri2', 'Track 2');
    child2.sortOrder = 5;

    container.insertChildInOrder(child1);
    container.insertChildInOrder(child2);

    expect(container.children).toHaveLength(2);
    expect(container.children[0]).toBe(child1);
    expect(container.children[1]).toBe(child2);
  });

  test('inserts multiple children and maintains order', () => {
    const child1 = new TrackNode('uri1', 'Track 1');
    child1.sortOrder = 15;
    const child2 = new TrackNode('uri2', 'Track 2');
    child2.sortOrder = 10;
    const child3 = new TrackNode('uri3', 'Track 3');
    child3.sortOrder = 20;

    container.insertChildInOrder(child1);
    container.insertChildInOrder(child2);
    container.insertChildInOrder(child3);

    expect(container.children).toHaveLength(3);
    expect(container.children[0]).toBe(child2);
    expect(container.children[1]).toBe(child1);
    expect(container.children[2]).toBe(child3);
  });

  test('inserts a child with undefined sortOrder as 0', () => {
    const child1 = new TrackNode('uri1', 'Track 1');
    child1.sortOrder = 10;

    // sortOrder is undefined, treated as 0
    const child2 = new TrackNode('uri2', 'Track 2');

    container.insertChildInOrder(child1);
    container.insertChildInOrder(child2);

    expect(container.children).toHaveLength(2);

    // child2 (sortOrder 0) should be first
    expect(container.children[0]).toBe(child2);
    expect(container.children[1]).toBe(child1);
  });
});
