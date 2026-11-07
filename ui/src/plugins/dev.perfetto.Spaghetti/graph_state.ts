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

import {produce} from 'immer';
import type {NodeQueryBuilderStore, RootNodeData} from './graph_model';
import {
  deserializeStore,
  flattenNodes,
  removeNodeFromGraph,
  serializeStore,
} from './graph_utils';
import type {SqlDialect} from './sql_types';

const STORAGE_KEY = 'perfettoSpaghetti';
const MAX_HISTORY = 50;

export interface PendingGraphLayout {
  readonly autoLayout: boolean;
  readonly recenter: boolean;
}

export interface ReplaceStoreOptions {
  readonly pushHistory?: boolean;
  readonly selectedNodeId?: string;
  readonly autoLayout?: boolean;
  readonly recenter?: boolean;
}

export interface SpaghettiGraphStateOptions {
  readonly persist?: boolean;
}

/**
 * Trace-scoped state container for the Spaghetti query builder graph, undo/redo
 * history, and active node selection. Shared between SpaghettiPage and
 * Intelletto assistant tools so programmatic graph edits stay live-synced with
 * the canvas and support undo/redo.
 */
export class SpaghettiGraphState {
  private _store: NodeQueryBuilderStore = {
    nodes: [],
    labels: [],
  };
  private readonly history: NodeQueryBuilderStore[] = [this._store];
  private historyIndex = 0;
  private pendingLayout: PendingGraphLayout | undefined;
  private readonly persist: boolean;

  readonly selectedNodeIds = new Set<string>();
  pinnedNodeId: string | undefined;
  dialect: SqlDialect = 'sql';

  constructor(options?: SpaghettiGraphStateOptions) {
    this.persist = options?.persist ?? true;
    if (this.persist) {
      this.loadFromStorage();
    }
  }

  get store(): NodeQueryBuilderStore {
    return this._store;
  }

  getActiveNodeId(): string | undefined {
    if (this.pinnedNodeId !== undefined) {
      return this.pinnedNodeId;
    }
    if (this.selectedNodeIds.size === 1) {
      return this.selectedNodeIds.values().next().value;
    }
    return undefined;
  }

  consumePendingLayout(): PendingGraphLayout | undefined {
    const pending = this.pendingLayout;
    this.pendingLayout = undefined;
    return pending;
  }

  updateStore(
    updater: (draft: NodeQueryBuilderStore) => void,
    options?: {readonly skipHistory?: boolean},
  ): void {
    const nextStore = produce(this._store, updater);
    if (nextStore === this._store) return;
    this._store = nextStore;

    if (options?.skipHistory) {
      this.history[this.historyIndex] = this._store;
    } else {
      this.pushHistoryEntry(this._store);
    }

    this.saveToStorage();
  }

  replaceStore(
    nextStore: NodeQueryBuilderStore,
    options?: ReplaceStoreOptions,
  ): void {
    this._store = nextStore;
    if (options?.pushHistory ?? false) {
      this.pushHistoryEntry(nextStore);
    } else {
      this.history.splice(0, this.history.length, nextStore);
      this.historyIndex = 0;
    }

    this.selectedNodeIds.clear();
    this.pinnedNodeId = undefined;

    if (options?.selectedNodeId !== undefined) {
      const index = flattenNodes(nextStore.nodes);
      if (index.nodes[options.selectedNodeId] !== undefined) {
        this.selectedNodeIds.add(options.selectedNodeId);
      }
    }

    if (options?.autoLayout || options?.recenter) {
      this.pendingLayout = {
        autoLayout: options.autoLayout ?? false,
        recenter: options.recenter ?? false,
      };
    }

    this.saveToStorage();
  }

  clear(): void {
    this._store = {nodes: [], labels: []};
    this.history.splice(0, this.history.length, this._store);
    this.historyIndex = 0;
    this.selectedNodeIds.clear();
    this.pinnedNodeId = undefined;
    this.pendingLayout = undefined;
    this.saveToStorage();
  }

  undo(): void {
    const prev = this.history[this.historyIndex - 1];
    if (prev !== undefined) {
      this.historyIndex--;
      this._store = prev;
      this.pruneMissingSelection();
      this.saveToStorage();
    }
  }

  redo(): void {
    const next = this.history[this.historyIndex + 1];
    if (next !== undefined) {
      this.historyIndex++;
      this._store = next;
      this.pruneMissingSelection();
      this.saveToStorage();
    }
  }

  canUndo(): boolean {
    return this.historyIndex > 0;
  }

  canRedo(): boolean {
    return this.historyIndex < this.history.length - 1;
  }

  updateNode(
    nodeId: string,
    updates: Partial<Omit<RootNodeData, 'id'>>,
    options?: {readonly skipHistory?: boolean},
  ): void {
    this.updateStore((draft) => {
      const node = draft.nodes.find((n) => n.id === nodeId);
      if (node) {
        Object.assign(node, updates);
      }
    }, options);
  }

  removeNode(nodeId: string): void {
    this.updateStore((draft) => {
      removeNodeFromGraph(draft.nodes, nodeId);
    });
    this.selectedNodeIds.delete(nodeId);
    if (this.pinnedNodeId === nodeId) {
      this.pinnedNodeId = undefined;
    }
  }

  loadFromStorage(): void {
    if (!this.persist || typeof localStorage === 'undefined') return;
    try {
      const saved = localStorage.getItem(STORAGE_KEY);
      if (!saved) return;
      this._store = deserializeStore(saved);
      this.history.splice(0, this.history.length, this._store);
      this.historyIndex = 0;
      this.selectedNodeIds.clear();
      this.pinnedNodeId = undefined;
    } catch (e) {
      console.error('[QueryBuilder] Failed to load graph:', e);
    }
  }

  saveToStorage(): void {
    if (!this.persist || typeof localStorage === 'undefined') return;
    try {
      localStorage.setItem(STORAGE_KEY, serializeStore(this._store));
    } catch (e) {
      console.error('[QueryBuilder] Failed to save graph:', e);
    }
  }

  private pushHistoryEntry(entry: NodeQueryBuilderStore): void {
    if (this.historyIndex < this.history.length - 1) {
      this.history.splice(this.historyIndex + 1);
    }
    this.history.push(entry);
    this.historyIndex = this.history.length - 1;
    if (this.history.length > MAX_HISTORY) {
      this.history.shift();
      this.historyIndex--;
    }
  }

  private pruneMissingSelection(): void {
    const index = flattenNodes(this._store.nodes);
    for (const id of [...this.selectedNodeIds]) {
      if (index.nodes[id] === undefined) {
        this.selectedNodeIds.delete(id);
      }
    }
    if (
      this.pinnedNodeId !== undefined &&
      index.nodes[this.pinnedNodeId] === undefined
    ) {
      this.pinnedNodeId = undefined;
    }
  }
}
