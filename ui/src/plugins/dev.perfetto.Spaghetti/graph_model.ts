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

import type {Label} from '../../widgets/nodegraph';

// ============================================================
// Runtime graph model (internal, used by the node editor)
// ============================================================

/**
 * A node in a chain. Has no canvas position — position is only meaningful
 * for root nodes. Chains nest via `next`, mirroring the widget's
 * Node / DockedNode split.
 */
export interface NodeData<C extends object = {}> {
  readonly id: string;
  readonly type: string;
  config: C;
  next?: NodeData;
  collapsed?: boolean;
  /**
   * Wired input connections indexed by port number. Each entry is the node ID
   * of the upstream node connected to that port, or null if the port slot
   * exists but has no connection. The docked parent (via next/chain) is NOT
   * represented here — it provides port 0 implicitly.
   *
   * Examples:
   *   []              – no wired connections (or only docked input)
   *   ['foo', 'bar']  – port 0 comes from node 'foo', port 1 from 'bar'
   *   [null, 'foo']   – port 0 is unconnected, port 1 comes from 'foo'
   */
  inputs?: (string | null)[];
}

/**
 * A root node. Only root nodes have canvas positions; all other nodes in
 * a chain are nested under `next` and inherit their parent's position.
 */
export interface RootNodeData<C extends object = {}> extends NodeData<C> {
  x: number;
  y: number;
}

export interface Port {
  /** Stable identifier used by getInputColumns / getInputRef. */
  readonly name: string;
  /** User-facing label shown on the port. Not used for programmatic lookup. */
  readonly content: string;
}

export interface NodeQueryBuilderStore {
  /** Root nodes in insertion order. Chain nodes are nested under root.next.next... */
  readonly nodes: RootNodeData[];
  readonly labels: Label[];
}

// ============================================================
// Spaghetti graph — the in-memory model
//
// `SpaghettiGraph` is the single source of truth at runtime,
// used for editing, undo/redo, serialization, and LLM authoring.
//
// A graph is a record of named stacks. Each stack is a linear
// sequence of ops (steps). Ops that take external inputs (join,
// union, interval_intersect, sql, ...) reference other stacks by
// name via StackRef values inside their config.
//
// A StackRef of null means the port slot exists but is unconnected.
// The length of an inputs array is therefore the number of ports
// the user has added, not the number of connected ports.
// ============================================================

/**
 * A reference to another stack by name, or null if the port slot
 * exists but has no connection yet.
 */
export type StackRef = string | null;

/** A single op step. The single key is the op type; the value is its config. */
export type SpaghettiStep = Record<string, unknown>;

/** The in-memory model for a Spaghetti query graph. */
export interface SpaghettiGraph {
  /** Named stacks. Each key is a stable ID used as a StackRef by other stacks. */
  stacks: Record<string, SpaghettiStep[]>;
  /** UI-only canvas state. The logic in `stacks` is the source of truth. */
  ui?: Record<string, {x: number; y: number; collapsed?: boolean}>;
}
