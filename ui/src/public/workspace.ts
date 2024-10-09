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

import {assertTrue} from '../base/logging';
import {raf} from '../core/raf_scheduler';

export interface WorkspaceManager {
  // This is the same of ctx.workspace, exposed for consistency also here.
  readonly currentWorkspace: Workspace;
  readonly all: ReadonlyArray<Workspace>;
  createEmptyWorkspace(displayName: string): Workspace;
  switchWorkspace(workspace: Workspace): void;
}

let sessionUniqueIdCounter = 0;

/**
 * Creates a short ID which is unique to this instance of the UI.
 *
 * The advantage of using this over uuidv4() is that the ids produced are
 * significantly shorter, saving memory and making them more human
 * read/write-able which helps when debugging.
 *
 * Note: The ID range will reset every time the UI is restarted, so be careful
 * not rely on these IDs in any medium that can survive between UI instances.
 *
 * TODO(stevegolton): We could possibly move this into its own module and use it
 * everywhere where session-unique ids are required.
 */
function createSessionUniqueId(): string {
  // Return the counter in base36 (0-z) to keep the string as short as possible
  // but still human readable.
  return (sessionUniqueIdCounter++).toString(36);
}

/**
 * Describes generic parent track node functionality - i.e. any entity that can
 * contain child TrackNodes, providing methods to add, remove, and access child
 * nodes.
 *
 * This class is abstract because, while it can technically be instantiated on
 * its own (no abstract methods/properties), it can't and shouldn't be
 * instantiated anywhere in practice - all APIs require either a TrackNode or a
 * Workspace.
 *
 * Thus, it serves two purposes:
 * 1. Avoiding duplication between Workspace and TrackNode, which is an internal
 *    implementation detail of this module.
 * 2. Providing a typescript interface for a generic TrackNode container class,
 *    which otherwise you might have to achieve using `Workspace | TrackNode`
 *    which is uglier.
 *
 * If you find yourself using this as a Javascript class in external code, e.g.
 * `instance of TrackNodeContainer`, you're probably doing something wrong.
 */
export abstract class TrackNodeContainer {
  protected _children: Array<TrackNode> = [];
  protected readonly tracksById = new Map<string, TrackNode>();

  /**
   * True if this node has children, false otherwise.
   */
  get hasChildren(): boolean {
    return this._children.length > 0;
  }

  /**
   * The ordered list of children belonging to this node.
   */
  get children(): ReadonlyArray<TrackNode> {
    return this._children;
  }

  /**
   * Inserts a new child node considering it's sortOrder.
   *
   * The child will be added before the first child whose |sortOrder| is greater
   * than the child node's sort order, or at the end if one does not exist. If
   * |sortOrder| is omitted on either node in the comparison it is assumed to be
   * 0.
   *
   * @param child - The child node to add.
   */
  addChildInOrder(child: TrackNode): void {
    const insertPoint = this._children.find(
      (n) => (n.sortOrder ?? 0) > (child.sortOrder ?? 0),
    );
    if (insertPoint) {
      this.addChildBefore(child, insertPoint);
    } else {
      this.addChildLast(child);
    }
  }

  /**
   * Add a new child node at the start of the list of children.
   *
   * @param child The new child node to add.
   */
  addChildLast(child: TrackNode): void {
    this.adopt(child);
    this._children.push(child);
    raf.scheduleFullRedraw();
  }

  /**
   * Add a new child node at the end of the list of children.
   *
   * @param child The child node to add.
   */
  addChildFirst(child: TrackNode): void {
    this.adopt(child);
    this._children.unshift(child);
    raf.scheduleFullRedraw();
  }

  /**
   * Add a new child node before an existing child node.
   *
   * @param child The child node to add.
   * @param referenceNode An existing child node. The new node will be added
   * before this node.
   */
  addChildBefore(child: TrackNode, referenceNode: TrackNode): void {
    if (child === referenceNode) return;

    assertTrue(this.children.includes(referenceNode));

    this.adopt(child);

    const indexOfReference = this.children.indexOf(referenceNode);
    this._children.splice(indexOfReference, 0, child);
    raf.scheduleFullRedraw();
  }

  /**
   * Add a new child node after an existing child node.
   *
   * @param child The child node to add.
   * @param referenceNode An existing child node. The new node will be added
   * after this node.
   */
  addChildAfter(child: TrackNode, referenceNode: TrackNode): void {
    if (child === referenceNode) return;

    assertTrue(this.children.includes(referenceNode));

    this.adopt(child);

    const indexOfReference = this.children.indexOf(referenceNode);
    this._children.splice(indexOfReference + 1, 0, child);
    raf.scheduleFullRedraw();
  }

  /**
   * Remove a child node from this node.
   *
   * @param child The child node to remove.
   */
  removeChild(child: TrackNode): void {
    this._children = this.children.filter((x) => child !== x);
    child.parent = undefined;
    child.id && this.tracksById.delete(child.id);
    raf.scheduleFullRedraw();
  }

  /**
   * The flattened list of all descendent nodes.
   */
  get flatTracks(): ReadonlyArray<TrackNode> {
    return this.children.flatMap((node) => {
      return [node, ...node.flatTracks];
    });
  }

  /**
   * Remove all children from this node.
   */
  clear(): void {
    this._children = [];
    this.tracksById.clear();
    raf.scheduleFullRedraw();
  }

  /**
   * Find a track node by its id.
   *
   * Node: This is an O(N) operation where N is the depth of the target node.
   * I.e. this is more efficient than findTrackByURI().
   *
   * @param id The id of the node we want to find.
   * @returns The node or undefined if no such node exists.
   */
  getTrackById(id: string): TrackNode | undefined {
    const foundNode = this.tracksById.get(id);
    if (foundNode) {
      return foundNode;
    } else {
      // Recurse our children
      for (const child of this._children) {
        const foundNode = child.getTrackById(id);
        if (foundNode) return foundNode;
      }
    }
    return undefined;
  }

  private adopt(child: TrackNode): void {
    if (child.parent) {
      child.parent.removeChild(child);
    }
    child.parent = this;
    child.id && this.tracksById.set(child.id, child);
  }
}

export interface TrackNodeArgs {
  title: string;
  id: string;
  uri: string;
  headless: boolean;
  sortOrder: number;
  collapsed: boolean;
  isSummary: boolean;
}

/**
 * A base class for any node with children (i.e. a group or a workspace).
 */
export class TrackNode extends TrackNodeContainer {
  // Immutable unique (within the workspace) ID of this track node. Used for
  // efficiently retrieving this node object from a workspace. Note: This is
  // different to |uri| which is used to reference a track to render on the
  // track. If this means nothing to you, don't bother using it.
  public readonly id: string;

  // Parent node - could be the workspace or another node.
  public parent?: TrackNodeContainer;

  // A human readable string for this track - displayed in the track shell.
  // TODO(stevegolton): Make this optional, so that if we implement a string for
  // this track then we can implement it here as well.
  public title: string;

  // The URI of the track content to display here.
  public uri?: string;

  // Optional sort order, which workspaces may or may not take advantage of for
  // sorting when displaying the workspace.
  public sortOrder?: number;

  // Don't show the header at all for this track, just show its un-nested
  // children. This is helpful to group together tracks that logically belong to
  // the same group (e.g. all ftrace cpu tracks) and ease the job of
  // sorting/grouping plugins.
  public headless: boolean;

  // If true, this track is to be used as a summary for its children. When the
  // group is expanded the track will become sticky to the top of the viewport
  // to provide context for the tracks within, and the content of this track
  // shall be omitted. It will also be squashed down to a smaller height to save
  // vertical space.
  public isSummary: boolean;

  protected _collapsed = true;

  constructor(args?: Partial<TrackNodeArgs>) {
    super();

    const {
      title = '',
      id = createSessionUniqueId(),
      uri,
      headless = false,
      sortOrder,
      collapsed = true,
      isSummary = false,
    } = args ?? {};

    this.id = id;
    this.uri = uri;
    this.headless = headless;
    this.title = title;
    this.sortOrder = sortOrder;
    this.isSummary = isSummary;
    this._collapsed = collapsed;
  }

  /**
   * Remove this track from it's parent & unpin from the workspace if pinned.
   */
  remove(): void {
    this.workspace?.unpinTrack(this);
    this.parent?.removeChild(this);
  }

  /**
   * Add this track to the list of pinned tracks in its parent workspace.
   *
   * Has no effect if this track is not added to a workspace.
   */
  pin(): void {
    this.workspace?.pinTrack(this);
  }

  /**
   * Remove this track from the list of pinned tracks in its parent workspace.
   *
   * Has no effect if this track is not added to a workspace.
   */
  unpin(): void {
    this.workspace?.unpinTrack(this);
  }

  /**
   * Returns true if this node is added to a workspace as is in the pinned track
   * list of that workspace.
   */
  get isPinned(): boolean {
    return Boolean(this.workspace?.hasPinnedTrack(this));
  }

  /**
   * Find the closest visible ancestor TrackNode.
   *
   * Given the path from the root workspace to this node, find the fist one,
   * starting from the root, which is collapsed. This will be, from the user's
   * point of view, the closest ancestor of this node.
   *
   * Returns undefined if this node is actually visible.
   *
   * TODO(stevegolton): Should it return itself in this case?
   */
  findClosestVisibleAncestor(): TrackNode {
    // Build a path from the root workspace to this node
    const path: TrackNode[] = [];
    let node = this.parent;
    while (node && node instanceof TrackNode) {
      path.unshift(node);
      node = node.parent;
    }

    // Find the first collapsed track in the path starting from the root. This
    // is effectively the closest we can get to this node without expanding any
    // groups.
    return path.find((node) => node.collapsed) ?? this;
  }

  /**
   * Expand all ancestor nodes.
   */
  reveal(): void {
    let parent = this.parent;
    while (parent && parent instanceof TrackNode) {
      parent.expand();
      parent = parent.parent;
    }
  }

  /**
   * Find this node's root node - this may be a workspace or another node.
   */
  get rootNode(): TrackNodeContainer | undefined {
    // Travel upwards through the tree to find the root node.
    let parent: TrackNodeContainer | undefined = this;
    while (parent && parent instanceof TrackNode) {
      parent = parent.parent;
    }
    return parent;
  }

  /**
   * Find this node's parent workspace if it is attached to one.
   */
  get workspace(): Workspace | undefined {
    // Find the root node and return it if it's a Workspace instance
    const rootNode = this.rootNode;
    if (rootNode instanceof Workspace) {
      return rootNode;
    }
    return undefined;
  }

  /**
   * Mark this node as un-collapsed, indicating its children should be rendered.
   */
  expand(): void {
    this._collapsed = false;
    raf.scheduleFullRedraw();
  }

  /**
   * Mark this node as collapsed, indicating its children should not be
   * rendered.
   */
  collapse(): void {
    this._collapsed = true;
    raf.scheduleFullRedraw();
  }

  /**
   * Toggle the collapsed state.
   */
  toggleCollapsed(): void {
    this._collapsed = !this._collapsed;
    raf.scheduleFullRedraw();
  }

  /**
   * Whether this node is collapsed, indicating its children should be rendered.
   */
  get collapsed(): boolean {
    return this._collapsed;
  }

  /**
   * Whether this node is expanded - i.e. not collapsed, indicating its children
   * should be rendered.
   */
  get expanded(): boolean {
    return !this._collapsed;
  }

  /**
   * Returns the list of titles representing the full path from the root node to
   * the current node. This path consists only of node titles, workspaces are
   * omitted.
   */
  get fullPath(): ReadonlyArray<string> {
    let fullPath = [this.title];
    let parent = this.parent;
    while (parent && parent instanceof TrackNode) {
      // Ignore headless containers as they don't appear in the tree...
      if (!parent.headless) {
        fullPath = [parent.title, ...fullPath];
      }
      parent = parent.parent;
    }
    return fullPath;
  }
}

/**
 * Defines a workspace containing a track tree and a pinned area.
 */
export class Workspace extends TrackNodeContainer {
  public title = '<untitled-workspace>';
  public readonly id: string;

  // Dummy node to contain the pinned tracks
  private pinnedRoot = new TrackNode();

  get pinnedTracks(): ReadonlyArray<TrackNode> {
    return this.pinnedRoot.children;
  }

  constructor() {
    super();
    this.id = createSessionUniqueId();
    this.pinnedRoot.parent = this;
  }

  /**
   * Reset the entire workspace including the pinned tracks.
   */
  override clear(): void {
    super.clear();
    this.pinnedRoot.clear();
  }

  /**
   * Adds a track node to this workspace's pinned area.
   */
  pinTrack(track: TrackNode): void {
    // Make a lightweight clone of this track - just the uri and the title.
    const cloned = new TrackNode({uri: track.uri, title: track.title});
    this.pinnedRoot.addChildLast(cloned);
  }

  /**
   * Removes a track node from this workspace's pinned area.
   */
  unpinTrack(track: TrackNode): void {
    const foundNode = this.pinnedRoot.children.find((t) => t.uri === track.uri);
    if (foundNode) {
      this.pinnedRoot.removeChild(foundNode);
    }
  }

  /**
   * Check if this workspace has a pinned track with the same URI as |track|.
   */
  hasPinnedTrack(track: TrackNode): boolean {
    return this.pinnedTracks.some((p) => p.uri === track.uri);
  }

  /**
   * Find a track node via its URI.
   *
   * Note: This in an O(N) operation where N is the number of nodes in the
   * workspace.
   *
   * @param uri The uri of the track to find.
   * @returns A reference to the track node if it exists in this workspace,
   * otherwise undefined.
   */
  findTrackByUri(uri: string): TrackNode | undefined {
    return this.flatTracks.find((t) => t.uri === uri);
  }

  /**
   * Find a track by ID, also searching pinned tracks.
   */
  override getTrackById(id: string): TrackNode | undefined {
    // Also search the pinned tracks
    return this.pinnedRoot.getTrackById(id) || super.getTrackById(id);
  }
}
