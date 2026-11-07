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

import type m from 'mithril';
import type {z} from 'zod';
import type {Trace} from '../../public/trace';
import type {SqlModules} from '../dev.perfetto.SqlModules/sql_modules';
import type {Port} from './graph_model';
import type {ColumnDef} from './graph_utils';

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
  readonly inputPorts: ReadonlyArray<Port>;
  /** Get the output columns of a specific input port by name. */
  getInputColumns(portName: string): ColumnDef[];
}

// ---------------------------------------------------------------------------
// Generators — each node renders itself in each SQL dialect.
// ---------------------------------------------------------------------------

/**
 * A node's contribution in Standard SQL, as structured clauses over its primary
 * input. The composer folds consecutive fragments into one SELECT block when
 * every clause added comes strictly later in SQL clause order
 * (FROM -> WHERE -> GROUP BY -> SELECT -> DISTINCT -> ORDER BY -> LIMIT) than
 * the block's latest clause (WHERE + WHERE is ANDed). Otherwise it starts a new
 * CTE. An empty fragment `{}` is a passthrough.
 */
export interface SqlFragment {
  /**
   * Custom source: a table name, a macro call, joins, or a `(subquery)`.
   * Defaults to the primary input. Always starts a new block. A block holding
   * only a bare-name `from` is rendered as a plain reference.
   */
  readonly from?: string;
  readonly where?: string;
  readonly groupBy?: readonly string[];
  readonly select?: readonly string[];
  readonly distinct?: boolean;
  readonly orderBy?: readonly string[];
  readonly limit?: number;
  readonly offset?: number;
  readonly includes?: readonly string[];
}

/**
 * A node's contribution in PerfettoSQL Next. Either `stages` appended to the
 * primary input's pipeline, or a new `source` clause (e.g. `FROM x`,
 * `INTERVAL INTERSECTION OF (...)`, `GRAPH ... TREE`) followed by `stages`.
 * An empty fragment `{}` is a passthrough.
 */
export interface PsnFragment {
  readonly source?: string;
  readonly stages?: readonly string[];
  readonly includes?: readonly string[];
}

/** Returned when a node can't be expressed in a dialect. */
export interface GeneratorError {
  readonly error: string;
}

/** An input as seen by the Standard SQL generator. */
export interface SqlInput {
  /** Always a relation name: a base table, a `_qb_` cache table or a CTE. */
  readonly ref: string;
}

/** An input as seen by the PerfettoSQL Next generator. */
export interface PsnInput {
  /** A table name or a parenthesized sub-pipeline, usable as a source. */
  readonly source: string;
  /** The input with extra stages appended to the end of its own pipeline. */
  withStages(stages: readonly string[]): string;
}

export interface ConnectedInput<I> {
  readonly port: string;
  readonly input: I;
}

export interface GeneratorContext<I> {
  /** Effective input ports for this node instance. */
  readonly inputPorts: ReadonlyArray<Port>;
  /** The input connected to a port, if any. */
  input(portName: string): I | undefined;
  /** All connected inputs, in port order. */
  inputs(): ReadonlyArray<ConnectedInput<I>>;
  /** Get the output columns of an input by port name. */
  getInputColumns(portName: string): ColumnDef[] | undefined;
  /** SQL modules for table/module metadata resolution. */
  readonly sqlModules: SqlModules | undefined;
}

export interface NodeGenerators<C> {
  /**
   * Standard SQL (required). Return undefined when the node is incomplete
   * (e.g. a required input is not connected).
   */
  sql(
    config: C,
    ctx: GeneratorContext<SqlInput>,
  ): SqlFragment | GeneratorError | undefined;
  /** PerfettoSQL Next (optional; omitted means unsupported in PSN mode). */
  psn?(
    config: C,
    ctx: GeneratorContext<PsnInput>,
  ): PsnFragment | GeneratorError | undefined;
}

// ---------------------------------------------------------------------------
// Details context — passed to renderDetails for custom details panel content.
// ---------------------------------------------------------------------------

export interface DetailsContext {
  /** Column definitions for this node's output. */
  readonly outputColumns: ColumnDef[] | undefined;
  /** Error from the last materialization, if any. */
  readonly error: string | undefined;
  /** The trace object (includes engine for running queries). */
  readonly trace: Trace;
  /**
   * The materialized output table name (e.g. '_qb_abcd1234'), queryable
   * directly via trace.engine. Undefined if not yet materialized.
   */
  readonly materializedTable: string | undefined;
}

// ---------------------------------------------------------------------------
// Column context — passed to getOutputColumns.
// ---------------------------------------------------------------------------

export interface ColumnContext {
  /** Effective input ports for this node instance. */
  readonly inputPorts: ReadonlyArray<Port>;
  /** Get the output columns of a specific input port by name. */
  getInputColumns(portName: string): ColumnDef[] | undefined;
  /** SQL modules for schema resolution (e.g. FROM node table lookup). */
  readonly sqlModules: SqlModules | undefined;
}

// ---------------------------------------------------------------------------
// Node manifest: single source of truth for each node type.
// ---------------------------------------------------------------------------

export interface NodeManifest<C extends object = object> {
  // Visual / graph metadatas
  readonly title: string;
  readonly icon: string;
  /** Optional config-dependent icon override. Method (bivariant) intentionally. */
  resolveIcon?(config: C): string;
  readonly hue: number;

  /** Zod schema used to validate and parse serialized node configs. */
  readonly schema: z.ZodType<C, unknown>;

  getInputs?(config: C): Port[];

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

  /** Render this node in each SQL dialect. */
  readonly generators: NodeGenerators<C>;

  /**
   * Potentially expensive nodes (joins, aggregates, interval ops...) have their
   * output materialized into a cache table. Everything else folds into the
   * next table's query or the output query.
   */
  readonly expensive?: boolean;

  /**
   * Optional: render custom content in the details panel instead of the
   * default DataGrid. Return undefined (or omit) to use the default DataGrid.
   */
  renderDetails?(config: C, ctx: DetailsContext): m.Children;
}
