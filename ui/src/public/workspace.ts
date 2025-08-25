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
import {errResult, okResult, Result} from '../base/result';

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
  return (sessionUniqueIdCounter++).toString();
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

export interface TrackNodeArgs {
  name: string;
  uri: string;
  headless: boolean;
  sortOrder: number;
  collapsed: boolean;
  isSummary: boolean;
  removable: boolean;
}

/**
 * A base class for any node with children (i.e. a group or a workspace).
 */
export class TrackNode {
  // Immutable unique (within the workspace) ID of this track node. Used for
  // efficiently retrieving this node object from a workspace. Note: This is
  // different to |uri| which is used to reference a track to render on the
  // track. If this means nothing to you, don't bother using it.
  public readonly id: string;

  // A human readable string for this track - displayed in the track shell.
  // TODO(stevegolton): Make this optional, so that if we implement a string for
  // this track then we can implement it here as well.
  public name: string;

  // The URI of the track content to display here.
  public uri?: string;

  // Optional sort order, which workspaces may or may not take advantage of for
  // sorting when displaying the workspace. Lower numbers appear first.
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

  // If true, this node will be removable by the user. It will show a little
  // close button in the track shell which the user can press to remove the
  // track from the workspace.
  public removable: boolean;

  protected _collapsed = true;
  protected _children: Array<TrackNode> = [];
  protected readonly tracksById = new Map<string, TrackNode>();
  protected readonly tracksByUri = new Map<string, TrackNode>();
  private _parent?: TrackNode;
  public _workspace?: Workspace;

  get parent(): TrackNode | undefined {
    return this._parent;
  }

  constructor(args?: Partial<TrackNodeArgs>) {
    const {
      name = '',
      uri,
      headless = false,
      sortOrder,
      collapsed = true,
      isSummary = false,
      removable = false,
    } = args ?? {};

    this.id = createSessionUniqueId();
    this.uri = uri;
    this.headless = headless;
    this.name = name;
    this.sortOrder = sortOrder;
    this.isSummary = isSummary;
    this._collapsed = collapsed;
    this.removable = removable;
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
    while (node) {
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
    while (parent) {
      parent.expand();
      parent = parent.parent;
    }
  }

  /**
   * Get all ancestors of this node from root to immediate parent.
   * Returns an empty array if this node has no parent.
   */
  getAncestors(): TrackNode[] {
    const ancestors: TrackNode[] = [];
    let current = this.parent;
    while (current && current.name !== '') {
      ancestors.push(current);
      current = current.parent;
    }
    return ancestors.reverse(); // Return from root to immediate parent
  }

  /**
   * Find this node's root node - this may be a workspace or another node.
   */
  get rootNode(): TrackNode {
    let node: TrackNode = this;
    while (node.parent) {
      node = node.parent;
    }
    return node;
  }

  /**
   * Find this node's workspace if it is attached to one.
   */
  get workspace(): Workspace | undefined {
    return this.rootNode._workspace;
  }

  /**
   * Mark this node as un-collapsed, indicating its children should be rendered.
   */
  expand(): void {
    this._collapsed = false;
  }

  /**
   * Mark this node as collapsed, indicating its children should not be
   * rendered.
   */
  collapse(): void {
    this._collapsed = true;
  }

  /**
   * Toggle the collapsed state.
   */
  toggleCollapsed(): void {
    this._collapsed = !this._collapsed;
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
    let fullPath = [this.name];
    let parent = this.parent;
    while (parent) {
      // Ignore headless containers as they don't appear in the tree...
      if (!parent.headless && parent.name !== '') {
        fullPath = [parent.name, ...fullPath];
      }
      parent = parent.parent;
    }
    return fullPath;
  }

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
  addChildInOrder(child: TrackNode): Result {
    const insertPoint = this._children.find(
      (n) => (n.sortOrder ?? 0) > (child.sortOrder ?? 0),
    );
    if (insertPoint) {
      return this.addChildBefore(child, insertPoint);
    } else {
      return this.addChildLast(child);
    }
  }

  /**
   * Add a new child node at the start of the list of children.
   *
   * @param child The new child node to add.
   */
  addChildLast(child: TrackNode): Result {
    const result = this.adopt(child);
    if (!result.ok) return result;
    this._children.push(child);
    return result;
  }

  /**
   * Add a new child node at the end of the list of children.
   *
   * @param child The child node to add.
   */
  addChildFirst(child: TrackNode): Result {
    const result = this.adopt(child);
    if (!result.ok) return result;
    this._children.unshift(child);
    return result;
  }

  /**
   * Add a new child node before an existing child node.
   *
   * @param child The child node to add.
   * @param referenceNode An existing child node. The new node will be added
   * before this node.
   */
  addChildBefore(child: TrackNode, referenceNode: TrackNode): Result {
    // Nodes are the same, nothing to do.
    if (child === referenceNode) return okResult();

    assertTrue(this.children.includes(referenceNode));

    const result = this.adopt(child);
    if (!result.ok) return result;

    const indexOfReference = this.children.indexOf(referenceNode);
    this._children.splice(indexOfReference, 0, child);

    return okResult();
  }

  /**
   * Add a new child node after an existing child node.
   *
   * @param child The child node to add.
   * @param referenceNode An existing child node. The new node will be added
   * after this node.
   */
  addChildAfter(child: TrackNode, referenceNode: TrackNode): Result {
    // Nodes are the same, nothing to do.
    if (child === referenceNode) return okResult();

    assertTrue(this.children.includes(referenceNode));

    const result = this.adopt(child);
    if (!result.ok) return result;

    const indexOfReference = this.children.indexOf(referenceNode);
    this._children.splice(indexOfReference + 1, 0, child);

    return okResult();
  }

  /**
   * Remove a child node from this node.
   *
   * @param child The child node to remove.
   */
  removeChild(child: TrackNode): void {
    this._children = this.children.filter((x) => child !== x);
    child._parent = undefined;
    this.removeFromIndex(child);
    this.propagateRemoval(child);
  }

  /**
   * The flattened list of all descendent nodes in depth first order.
   *
   * Use flatTracksUnordered if you don't care about track order, as it's more
   * efficient.
   */
  get flatTracksOrdered(): ReadonlyArray<TrackNode> {
    const tracks: TrackNode[] = [];
    this.collectFlatTracks(tracks);
    return tracks;
  }

  private collectFlatTracks(tracks: TrackNode[]): void {
    for (let i = 0; i < this.children.length; ++i) {
      tracks.push(this.children[i]); // Push the current node before its children
      this.children[i].collectFlatTracks(tracks); // Recurse to collect child tracks
    }
  }

  /**
   * The flattened list of all descendent nodes in no particular order.
   */
  get flatTracks(): ReadonlyArray<TrackNode> {
    return Array.from(this.tracksById.values());
  }

  /**
   * Remove all children from this node.
   */
  clear(): void {
    this._children = [];
    this.tracksById.clear();
  }

  /**
   * Get a track node by its id.
   *
   * Node: This is an O(1) operation.
   *
   * @param id The id of the node we want to find.
   * @returns The node or undefined if no such node exists.
   */
  getTrackById(id: string): TrackNode | undefined {
    return this.tracksById.get(id);
  }

  /**
   * Get a track node via its URI.
   *
   * Node: This is an O(1) operation.
   *
   * @param uri The uri of the track to find.
   * @returns The node or undefined if no such node exists with this URI.
   */
  getTrackByUri(uri: string): TrackNode | undefined {
    return this.tracksByUri.get(uri);
  }

  /**
   * Creates a copy of this node with a new ID.
   *
   * @param deep - If true, children are copied too.
   * @returns - A copy of this node.
   */
  clone(deep = false): TrackNode {
    const cloned = new TrackNode({...this, id: undefined});
    if (deep) {
      this.children.forEach((c) => {
        cloned.addChildLast(c.clone(deep));
      });
    }
    return cloned;
  }

  private adopt(child: TrackNode): Result {
    if (child === this || child.getTrackById(this.id)) {
      return errResult(
        'Cannot move track into itself or one of its descendants',
      );
    }

    if (child.parent) {
      child.parent.removeChild(child);
    }
    child._parent = this;
    this.addToIndex(child);
    this.propagateAddition(child);

    return okResult();
  }

  private addToIndex(child: TrackNode) {
    this.tracksById.set(child.id, child);
    for (const [id, node] of child.tracksById) {
      this.tracksById.set(id, node);
    }

    child.uri && this.tracksByUri.set(child.uri, child);
    for (const [uri, node] of child.tracksByUri) {
      this.tracksByUri.set(uri, node);
    }
  }

  private removeFromIndex(child: TrackNode) {
    this.tracksById.delete(child.id);
    for (const [id] of child.tracksById) {
      this.tracksById.delete(id);
    }

    child.uri && this.tracksByUri.delete(child.uri);
    for (const [uri] of child.tracksByUri) {
      this.tracksByUri.delete(uri);
    }
  }

  private propagateAddition(node: TrackNode): void {
    if (this.parent) {
      this.parent.addToIndex(node);
      this.parent.propagateAddition(node);
    }
  }

  private propagateRemoval(node: TrackNode): void {
    if (this.parent) {
      this.parent.removeFromIndex(node);
      this.parent.propagateRemoval(node);
    }
  }
}

/**
 * Defines a workspace containing a track tree and a pinned area.
 */
export class Workspace {
  public title = '<untitled-workspace>';
  public readonly id: string;
  public userEditable: boolean = true;

  // Dummy node to contain the pinned tracks
  public readonly pinnedTracksNode = new TrackNode();
  public readonly tracks = new TrackNode();

  get pinnedTracks(): ReadonlyArray<TrackNode> {
    return this.pinnedTracksNode.children;
  }

  constructor() {
    this.id = createSessionUniqueId();
    this.pinnedTracksNode._workspace = this;
    this.tracks._workspace = this;

    // Expanding these nodes makes the logic work
    this.pinnedTracksNode.expand();
    this.tracks.expand();
  }

  /**
   * Reset the entire workspace including the pinned tracks.
   */
  clear(): void {
    this.pinnedTracksNode.clear();
    this.tracks.clear();
  }

  /**
   * Adds a track node to this workspace's pinned area.
   */
  pinTrack(track: TrackNode): void {
    // Make a lightweight clone of this track - just the uri and the title.
    const cloned = new TrackNode({
      uri: track.uri,
      name: track.name,
      removable: track.removable,
    });
    this.pinnedTracksNode.addChildLast(cloned);
  }

  /**
   * Removes a track node from this workspace's pinned area.
   */
  unpinTrack(track: TrackNode): void {
    const foundNode = this.pinnedTracksNode.children.find(
      (t) => t.uri === track.uri,
    );
    if (foundNode) {
      this.pinnedTracksNode.removeChild(foundNode);
    }
  }

  /**
   * Check if this workspace has a pinned track with the same URI as |track|.
   */
  hasPinnedTrack(track: TrackNode): boolean {
    return this.pinnedTracksNode.flatTracks.some((p) => p.uri === track.uri);
  }

  /**
   * Get a track node via its URI.
   *
   * Node: This is an O(1) operation.
   *
   * @param uri The uri of the track to find.
   * @returns The node or undefined if no such node exists with this URI.
   */
  getTrackByUri(uri: string): TrackNode | undefined {
    return this.tracks.flatTracks.find((t) => t.uri === uri);
  }

  /**
   * Get a track node by its id.
   *
   * Node: This is an O(1) operation.
   *
   * @param id The id of the node we want to find.
   * @returns The node or undefined if no such node exists.
   */
  getTrackById(id: string): TrackNode | undefined {
    return (
      this.tracks.getTrackById(id) || this.pinnedTracksNode.getTrackById(id)
    );
  }

  /**
   * The ordered list of children belonging to this node.
   */
  get children(): ReadonlyArray<TrackNode> {
    return this.tracks.children;
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
  addChildInOrder(child: TrackNode): Result {
    return this.tracks.addChildInOrder(child);
  }

  /**
   * Add a new child node at the start of the list of children.
   *
   * @param child The new child node to add.
   */
  addChildLast(child: TrackNode): Result {
    return this.tracks.addChildLast(child);
  }

  /**
   * Add a new child node at the end of the list of children.
   *
   * @param child The child node to add.
   */
  addChildFirst(child: TrackNode): Result {
    return this.tracks.addChildFirst(child);
  }

  /**
   * Add a new child node before an existing child node.
   *
   * @param child The child node to add.
   * @param referenceNode An existing child node. The new node will be added
   * before this node.
   */
  addChildBefore(child: TrackNode, referenceNode: TrackNode): Result {
    return this.tracks.addChildBefore(child, referenceNode);
  }

  /**
   * Add a new child node after an existing child node.
   *
   * @param child The child node to add.
   * @param referenceNode An existing child node. The new node will be added
   * after this node.
   */
  addChildAfter(child: TrackNode, referenceNode: TrackNode): Result {
    return this.tracks.addChildAfter(child, referenceNode);
  }

  /**
   * Remove a child node from this node.
   *
   * @param child The child node to remove.
   */
  removeChild(child: TrackNode): void {
    this.tracks.removeChild(child);
  }

  /**
   * The flattened list of all descendent nodes in depth first order.
   *
   * Use flatTracksUnordered if you don't care about track order, as it's more
   * efficient.
   */
  get flatTracksOrdered() {
    return this.tracks.flatTracksOrdered;
  }

  /**
   * The flattened list of all descendent nodes in no particular order.
   */
  get flatTracks() {
    return this.tracks.flatTracks;
  }
}
