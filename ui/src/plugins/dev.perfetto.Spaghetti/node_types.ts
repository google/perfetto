// Copyright (C) 2025 The Android Open Source Project
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

import m from 'mithril';
import {Connection, Label, NodePort} from '../../widgets/nodegraph';
import {Trace} from '../../public/trace';
import {ColumnDef} from './graph_utils';
import {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';

// ---------------------------------------------------------------------------
// Node data: separates graph topology from node-specific config.
// ---------------------------------------------------------------------------

/** Graph-level data shared by every node. */
export interface NodeData<C extends object = {}> {
  readonly type: string;
  readonly id: string;
  x: number;
  y: number;
  nextId?: string;
  collapsed?: boolean;
  /** Stored input ports for variable-input nodes. Absent for static-input nodes. */
  inputs?: ManifestPort[];
  config: C;
}

// ---------------------------------------------------------------------------
// SQL statement (used by tryFold in node manifests).
// ---------------------------------------------------------------------------

export interface SqlStatement {
  distinct?: boolean;
  columns: string;
  from: string;
  where?: string;
  groupBy?: string;
  orderBy?: string;
  limit?: number;
}

// ---------------------------------------------------------------------------
// Manifest port: NodePort with a stable name for programmatic lookup.
// ---------------------------------------------------------------------------

export interface ManifestPort extends NodePort {
  /** Stable identifier used by getInputColumns / getInputRef. */
  readonly name: string;
  /** Direction for connection compatibility and port placement. */
  readonly direction: 'top' | 'left' | 'right' | 'bottom';
  /** User-facing label shown on the port. Not used for programmatic lookup. */
  readonly content: string;
}

// ---------------------------------------------------------------------------
// Render context bag — each render function picks what it needs.
// ---------------------------------------------------------------------------

export interface RenderContext {
  /** Columns available from the primary (upstream) input. */
  readonly availableColumns: ColumnDef[];
  /** All table names for FROM node pickers. */
  readonly tableNames: string[];
  /** The trace object for timeline access. */
  readonly trace: Trace;
  /** Whether this node is currently selected. */
  readonly isSelected: boolean;
  /** Effective input ports for this node instance. */
  readonly inputPorts: ReadonlyArray<ManifestPort>;
  /** Get the output columns of a specific input port by name. */
  getInputColumns(portName: string): ColumnDef[];
  /** For variable-input nodes: add a new input port. */
  readonly addInput?: (port: ManifestPort) => void;
  /** For variable-input nodes: remove the last input port (and its connections). */
  readonly removeLastInput?: () => void;
}

// ---------------------------------------------------------------------------
// IR context — passed to emitIr for standalone SQL emission.
// ---------------------------------------------------------------------------

export interface IrContext {
  /** Effective input ports for this node instance. */
  readonly inputPorts: ReadonlyArray<ManifestPort>;
  /** Get the SQL reference (table name or CTE hash) for an input by port name. */
  getInputRef(portName: string): string;
  /** Get the output columns of an input by port name. */
  getInputColumns(portName: string): ColumnDef[] | undefined;
}

// ---------------------------------------------------------------------------
// Column context — passed to getOutputColumns.
// ---------------------------------------------------------------------------

export interface ColumnContext {
  /** Effective input ports for this node instance. */
  readonly inputPorts: ReadonlyArray<ManifestPort>;
  /** Get the output columns of a specific input port by name. */
  getInputColumns(portName: string): ColumnDef[] | undefined;
  /** SQL modules for schema resolution (e.g. FROM node table lookup). */
  readonly sqlModules: SqlModules | undefined;
}

// ---------------------------------------------------------------------------
// Node manifest: single source of truth for each node type.
// ---------------------------------------------------------------------------

export interface NodeManifest<C extends object = {}> {
  // Visual / graph metadata (previously in NODE_CONFIGS).
  readonly title: string;
  readonly icon?: string;
  readonly inputs?: ReadonlyArray<ManifestPort>;
  readonly outputs?: ReadonlyArray<ManifestPort>;
  readonly canDockTop?: boolean;
  readonly canDockBottom?: boolean;
  readonly hue: number;

  /**
   * Factory for the initial input port list. If present, the node supports
   * variable inputs: the returned list is stored in NodeData.inputs on creation
   * and mutated via RenderContext.addInput / removeLastInput.
   * If absent, the static `inputs` array is the source of truth.
   */
  defaultInputs?(): ManifestPort[];

  /** Return the default config for a newly-created node. */
  defaultConfig(): C;

  /** Render the node body. */
  render(
    config: C,
    updateConfig: (updates: Partial<C>) => void,
    ctx: RenderContext,
  ): m.Children;

  /** Pure config validation — no connection context. */
  isValid(config: C): boolean;

  /** Compute output columns (optional — absent means pass-through). */
  getOutputColumns?(config: C, ctx: ColumnContext): ColumnDef[] | undefined;

  /** Try to fold this node into the current SQL statement (optional). */
  tryFold?(stmt: SqlStatement, config: C): boolean;

  /**
   * Emit standalone SQL for nodes that can't fold (e.g. from, join, union).
   * Returns {sql, includes} or undefined to fall back to generic fold logic.
   */
  emitIr?(
    config: C,
    ctx: IrContext,
  ): {sql: string; includes?: string[]} | undefined;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

export interface NodeQueryBuilderStore {
  readonly nodes: Map<string, NodeData>;
  readonly connections: Connection[];
  readonly labels: Label[];
}
