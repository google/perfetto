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

import {Optional} from '../base/utils';
import {uuidv4} from '../base/uuid';
import {raf} from '../core/raf_scheduler';

export class TrackNode {
  // This is the URI of the track this node references.
  public readonly uri: string;
  public displayName: string;
  public parent?: ContainerNode;

  constructor(uri: string, displayName: string) {
    this.uri = uri;
    this.displayName = displayName;
  }

  // Expand all ancestors
  reveal(): void {
    let parent = this.parent;
    while (parent && parent instanceof GroupNode) {
      parent.expand();
      parent = parent.parent;
    }
  }

  get workspace(): Optional<Workspace> {
    let parent = this.parent;
    while (parent && !(parent instanceof Workspace)) {
      parent = parent.parent;
    }
    return parent;
  }

  remove(): void {
    this.workspace?.unpinTrack(this);
    this.parent?.removeChild(this);
  }

  pin(): void {
    this.workspace?.pinTrack(this);
  }

  unpin(): void {
    this.workspace?.unpinTrack(this);
  }

  get isPinned(): boolean {
    return Boolean(this.workspace?.pinnedTracks.includes(this));
  }

  get closestVisibleAncestor(): Optional<GroupNode> {
    // Build a path back up to the root.
    const path: ContainerNode[] = [];
    let group = this.parent;
    while (group) {
      path.unshift(group);
      group = group.parent;
    }

    // Find the first collapsed group in the path starting from the root.
    // This will be the last ancestor which isn't collapsed behind a group.
    for (const p of path) {
      if (p instanceof GroupNode && p.collapsed) {
        return p;
      }
    }

    return undefined;
  }
}

/**
 * A base class for any node with children (i.e. a group or a workspace).
 */
export class ContainerNode {
  public displayName: string;
  public parent?: ContainerNode;
  private _children: Array<Node>;

  clear(): void {
    this._children = [];
  }

  get children(): ReadonlyArray<Node> {
    return this._children;
  }

  constructor(displayName: string) {
    this.displayName = displayName;
    this._children = [];
  }

  private adopt(child: Node): void {
    if (child.parent) {
      child.parent.removeChild(child);
    }
    child.parent = this;
  }

  addChild(child: Node): void {
    this.adopt(child);
    this._children.push(child);
    raf.scheduleFullRedraw();
  }

  prependChild(child: Node): void {
    this.adopt(child);
    this._children.unshift(child);
    raf.scheduleFullRedraw();
  }

  removeChild(child: Node): void {
    this._children = this.children.filter((x) => child !== x);
    child.parent = undefined;
    raf.scheduleFullRedraw();
  }

  insertBefore(newNode: Node, referenceNode: Node): void {
    const indexOfReference = this.children.indexOf(referenceNode);
    if (indexOfReference === -1) {
      throw new Error('Reference node is not a child of this node');
    }

    if (newNode.parent) {
      newNode.parent.removeChild(newNode);
    }
    newNode.parent = this;

    this._children.splice(indexOfReference, 0, newNode);
    raf.scheduleFullRedraw();
  }

  insertAfter(newNode: Node, referenceNode: Node): void {
    const indexOfReference = this.children.indexOf(referenceNode);
    if (indexOfReference === -1) {
      throw new Error('Reference node is not a child of this node');
    }

    if (newNode.parent) {
      newNode.parent.removeChild(newNode);
    }
    newNode.parent = this;

    this._children.splice(indexOfReference + 1, 0, newNode);
    raf.scheduleFullRedraw();
  }

  /**
   * Returns an array containing the flattened list of all nodes (tracks and
   * groups) within this node.
   */
  get flatNodes(): ReadonlyArray<Node> {
    const nodes = this.children.flatMap((node) => {
      if (node instanceof TrackNode) {
        return node;
      } else {
        return [node, ...node.flatNodes];
      }
    });
    return nodes;
  }

  /**
   * Returns an array containing the flattened list of tracks within this node.
   */
  get flatTracks(): ReadonlyArray<TrackNode> {
    return this.flatNodes.filter((t) => t instanceof TrackNode);
  }

  /**
   * Returns an array containing the flattened list of groups within this
   * workspace.
   */
  get flatGroups(): ReadonlyArray<GroupNode> {
    return this.flatNodes.filter((t) => t instanceof GroupNode);
  }
}

export class GroupNode extends ContainerNode {
  // A unique URI used to identify this group
  public readonly uri: string;

  // Optional URI of a track to show on this group's header.
  public headerTrackUri?: string;

  // If true, this track will not show a header & permanently expanded.
  public headless: boolean;

  private _collapsed: boolean;

  constructor(displayName: string) {
    super(displayName);
    this._collapsed = true;
    this.headless = false;
    this.uri = uuidv4();
  }

  expand(): void {
    this._collapsed = false;
    raf.scheduleFullRedraw();
  }

  collapse(): void {
    this._collapsed = true;
    raf.scheduleFullRedraw();
  }

  toggleCollapsed(): void {
    this._collapsed = !this._collapsed;
    raf.scheduleFullRedraw();
  }

  get collapsed(): boolean {
    return this._collapsed;
  }

  get expanded(): boolean {
    return !this._collapsed;
  }
}

export type Node = TrackNode | GroupNode;

/**
 * Defines a workspace containing a track tree and a pinned area.
 */
export class Workspace extends ContainerNode {
  public pinnedTracks: Array<TrackNode>;
  public readonly uuid: string;

  constructor(displayName: string) {
    super(displayName);
    this.pinnedTracks = [];
    this.uuid = uuidv4();
  }

  /**
   * Reset the entire workspace including the pinned tracks.
   */
  clear(): void {
    this.pinnedTracks = [];
    super.clear();
    raf.scheduleFullRedraw();
  }

  /**
   * Adds a track node to this workspace's pinned area.
   */
  pinTrack(track: TrackNode): void {
    // TODO(stevegolton): Check if the track exists in this workspace first
    // otherwise we might get surprises.
    this.pinnedTracks.push(track);
    raf.scheduleFullRedraw();
  }

  /**
   * Removes a track node from this workspace's pinned area.
   */
  unpinTrack(track: TrackNode): void {
    this.pinnedTracks = this.pinnedTracks.filter((t) => t !== track);
    raf.scheduleFullRedraw();
  }

  /**
   * Get a track node by its URI.
   *
   * @param uri - The URI of the track to look up.
   * @returns The track node if it exists in this workspace, otherwise
   * undefined.
   */
  getTrackByUri(uri: string): Optional<TrackNode> {
    return this.flatTracks.find((t) => t.uri === uri);
  }
}
