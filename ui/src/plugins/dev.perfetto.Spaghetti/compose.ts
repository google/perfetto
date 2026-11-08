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

// Composes node generator fragments into a single query.
//
// Standard SQL: consecutive clause fragments fold into one SELECT block when
// every clause they add comes strictly later in SQL clause order than the
// block's latest clause (WHERE + WHERE is ANDed). Otherwise the block is
// emitted as a CTE and a new block starts reading from it.
//
// PerfettoSQL Next: stage fragments are appended to their input's pipeline;
// source fragments start a new pipeline. Pipelines used more than once are
// repeated (PSN has no CTEs).

import type {
  GeneratorError,
  PsnFragment,
  PsnInput,
  SqlFragment,
  SqlInput,
} from './node_types';
import {
  isEmptyFragment,
  makeGeneratorContext,
  type Plan,
  type PlanNode,
} from './plan';
import type {SqlDialect} from './sql_types';

export interface FormatOptions {
  readonly includeModuleHeaders?: boolean;
}

export type FormatResult =
  | {
      readonly ok: true;
      readonly sql: string;
      readonly includes: readonly string[];
      /** Set when the query is just a read of a single named relation. */
      readonly relation?: string;
    }
  | {
      readonly ok: false;
      readonly error: string;
    };

/** Which nodes to compose, and how to name nodes outside the set. */
export interface ComposeSpec {
  /** Nodes rendered inline, in plan (topological) order. */
  readonly members: ReadonlySet<string>;
  /** The node whose output the query returns. */
  readonly rootId: string;
  /** Table name for a referenced node outside `members`. */
  readonly tableFor: (nodeId: string) => string;
}

const IDENT_RE = /^[A-Za-z_][A-Za-z0-9_]*$/;

function indent(text: string, prefix = '  '): string {
  return text
    .split('\n')
    .map((line) => (line === '' ? line : `${prefix}${line}`))
    .join('\n');
}

function generatorError(node: PlanNode, error: string): FormatResult {
  return {ok: false, error: `${node.manifest.title}: ${error}`};
}

function isError<F extends object>(
  frag: F | GeneratorError,
): frag is GeneratorError {
  return 'error' in frag;
}

// Generator errors are self-describing user-facing messages.
function fromGeneratorError(frag: GeneratorError): FormatResult {
  return {ok: false, error: frag.error};
}

// --- Standard SQL ---

interface Block {
  readonly from: string;
  readonly where?: string;
  readonly groupBy?: readonly string[];
  readonly select?: readonly string[];
  readonly distinct?: boolean;
  readonly orderBy?: readonly string[];
  readonly limit?: number;
  readonly offset?: number;
}

// SQL evaluation order of each clause.
const CLAUSE_POS = {
  from: 0,
  where: 1,
  groupBy: 2,
  select: 3,
  distinct: 4,
  orderBy: 5,
  limit: 6,
  offset: 6,
} as const;

type Clause = keyof typeof CLAUSE_POS;

function clausesOf(x: Block | SqlFragment): Clause[] {
  return (Object.keys(CLAUSE_POS) as Clause[]).filter(
    (c) => x[c] !== undefined,
  );
}

function latestPos(block: Block): number {
  return Math.max(...clausesOf(block).map((c) => CLAUSE_POS[c]));
}

function canFold(block: Block, frag: SqlFragment): boolean {
  const latest = latestPos(block);
  return clausesOf(frag).every(
    (c) =>
      CLAUSE_POS[c] > latest || (c === 'where' && latest <= CLAUSE_POS.where),
  );
}

function foldInto(block: Block, frag: SqlFragment): Block {
  const where =
    block.where !== undefined && frag.where !== undefined
      ? `(${block.where}) AND (${frag.where})`
      : (block.where ?? frag.where);
  return {
    ...block,
    ...withoutUndefined({...frag, includes: undefined}),
    where,
  };
}

function withoutUndefined<T extends object>(obj: T): Partial<T> {
  return Object.fromEntries(
    Object.entries(obj).filter(([, v]) => v !== undefined),
  ) as Partial<T>;
}

function isBareRelation(block: Block): boolean {
  return clausesOf(block).length === 1 && IDENT_RE.test(block.from);
}

function renderBlock(block: Block): string {
  const cols = block.select ?? ['*'];
  const head = block.distinct ? 'SELECT DISTINCT' : 'SELECT';
  const lines = [
    cols.length > 1 ? `${head}\n  ${cols.join(',\n  ')}` : `${head} ${cols[0]}`,
    `FROM ${block.from}`,
  ];
  if (block.where !== undefined) lines.push(`WHERE ${block.where}`);
  if (block.groupBy !== undefined && block.groupBy.length > 0) {
    lines.push(`GROUP BY ${block.groupBy.join(', ')}`);
  }
  if (block.orderBy !== undefined && block.orderBy.length > 0) {
    lines.push(`ORDER BY ${block.orderBy.join(', ')}`);
  }
  if (block.limit !== undefined) {
    lines.push(
      block.offset !== undefined && block.offset > 0
        ? `LIMIT ${block.limit} OFFSET ${block.offset}`
        : `LIMIT ${block.limit}`,
    );
  }
  return lines.join('\n');
}

type SqlState =
  | {readonly kind: 'block'; readonly block: Block}
  | {readonly kind: 'name'; readonly name: string};

function composeSql(plan: Plan, spec: ComposeSpec): FormatResult {
  const states = new Map<string, SqlState>();
  const ctes: Array<{name: string; sql: string}> = [];
  const cteNames = new Map<string, string>();
  const typeCounters = new Map<string, number>();
  const includes = new Set<string>();
  const nodeById = new Map(plan.nodes.map((n) => [n.nodeId, n]));
  // Consumers within this query only, so the rendered SQL depends on nothing
  // outside the step (its hash is derived from this text).
  const localRefs = new Map<string, number>();
  for (const n of plan.nodes) {
    if (!spec.members.has(n.nodeId)) continue;
    for (const inp of n.inputs) {
      localRefs.set(inp.nodeId, (localRefs.get(inp.nodeId) ?? 0) + 1);
    }
  }

  function cteNameFor(nodeId: string): string {
    let name = cteNames.get(nodeId);
    if (name === undefined) {
      const type = nodeById.get(nodeId)?.node.type ?? 'step';
      const n = (typeCounters.get(type) ?? 0) + 1;
      typeCounters.set(type, n);
      name = `${type}_${n}`;
      cteNames.set(nodeId, name);
    }
    return name;
  }

  // The name an input will be referred to by (without emitting anything yet).
  function refNameOf(nodeId: string): string {
    if (!spec.members.has(nodeId)) return spec.tableFor(nodeId);
    const state = states.get(nodeId)!;
    if (state.kind === 'name') return state.name;
    if (isBareRelation(state.block)) return state.block.from;
    return cteNameFor(nodeId);
  }

  // Ensure an input is available by name, emitting its block as a CTE.
  function materializeName(nodeId: string): string {
    if (!spec.members.has(nodeId)) return spec.tableFor(nodeId);
    const state = states.get(nodeId)!;
    if (state.kind === 'name') return state.name;
    if (isBareRelation(state.block)) return state.block.from;
    const name = cteNameFor(nodeId);
    ctes.push({name, sql: renderBlock(state.block)});
    states.set(nodeId, {kind: 'name', name});
    return name;
  }

  for (const node of plan.nodes) {
    if (!spec.members.has(node.nodeId)) continue;

    const accessed = new Set<string>();
    const ctx = makeGeneratorContext<SqlInput>(
      node.ports,
      node.inputs.map((inp) => ({
        port: inp.port,
        input: {
          get ref() {
            accessed.add(inp.nodeId);
            return refNameOf(inp.nodeId);
          },
        },
      })),
      node.inputColumns,
      undefined,
    );
    const frag = node.manifest.generators.sql(node.node.config, ctx);
    if (frag === undefined) return generatorError(node, 'incomplete node');
    if (isError(frag)) return fromGeneratorError(frag);
    for (const inc of node.includes.sql) includes.add(inc);

    for (const id of accessed) materializeName(id);

    const primary = node.inputs[0]?.nodeId;
    if (isEmptyFragment(frag)) {
      if (primary === undefined) return generatorError(node, 'no input');
      states.set(
        node.nodeId,
        spec.members.has(primary)
          ? states.get(primary)!
          : {kind: 'name', name: spec.tableFor(primary)},
      );
      continue;
    }

    const primaryState =
      primary !== undefined && spec.members.has(primary)
        ? states.get(primary)
        : undefined;
    if (
      frag.from === undefined &&
      primary !== undefined &&
      primaryState?.kind === 'block' &&
      localRefs.get(primary) === 1 &&
      canFold(primaryState.block, frag)
    ) {
      states.set(node.nodeId, {
        kind: 'block',
        block: foldInto(primaryState.block, frag),
      });
      continue;
    }

    let from = frag.from;
    if (from === undefined) {
      if (primary === undefined) return generatorError(node, 'no input');
      from = materializeName(primary);
    }
    states.set(node.nodeId, {
      kind: 'block',
      block: foldInto({from}, {...frag, from: undefined}),
    });
  }

  const root = states.get(spec.rootId);
  if (root === undefined) return {ok: false, error: 'Empty plan.'};
  const relation =
    root.kind === 'name'
      ? root.name
      : isBareRelation(root.block)
        ? root.block.from
        : undefined;
  const body =
    root.kind === 'name'
      ? `SELECT *\nFROM ${root.name}`
      : renderBlock(root.block);
  if (ctes.length === 0) {
    return {ok: true, sql: body, includes: [...includes], relation};
  }
  const withSql = ctes
    .map((c) => `${c.name} AS (\n${indent(c.sql)}\n)`)
    .join(',\n');
  return {ok: true, sql: `WITH\n${withSql}\n${body}`, includes: [...includes]};
}

// --- PerfettoSQL Next ---

interface Pipeline {
  /** A full source clause, e.g. `FROM slice` or `INTERVAL INTERSECTION OF`. */
  readonly source: string;
  readonly stages: readonly string[];
}

function renderPipeline(p: Pipeline): string {
  return [p.source, ...p.stages.map((s) => `|> ${s}`)].join('\n');
}

/** The table name of a pipeline that is just `FROM <table>`. */
function bareTableOf(p: Pipeline): string | undefined {
  if (p.stages.length > 0) return undefined;
  return /^FROM ([A-Za-z_][A-Za-z0-9_]*)$/.exec(p.source)?.[1];
}

/** A pipeline as a source: a bare table name or a `(sub-pipeline)`. */
function pipelineAsSource(p: Pipeline): string {
  const bare = bareTableOf(p);
  if (bare !== undefined) return bare;
  // `FROM (<plain SQL>)` with no stages: the subquery is already a source.
  if (p.stages.length === 0 && /^FROM \([\s\S]*\)$/.test(p.source)) {
    return p.source.slice('FROM '.length);
  }
  return `(\n${indent(renderPipeline(p))}\n)`;
}

function composePsn(plan: Plan, spec: ComposeSpec): FormatResult {
  const states = new Map<string, Pipeline>();
  const includes = new Set<string>();

  function pipelineOf(nodeId: string): Pipeline {
    return spec.members.has(nodeId)
      ? states.get(nodeId)!
      : {source: `FROM ${spec.tableFor(nodeId)}`, stages: []};
  }

  for (const node of plan.nodes) {
    if (!spec.members.has(node.nodeId)) continue;
    const generate = node.manifest.generators.psn;
    if (generate === undefined) {
      return generatorError(node, 'not supported in PerfettoSQL Next mode');
    }

    const ctx = makeGeneratorContext<PsnInput>(
      node.ports,
      node.inputs.map((inp) => ({
        port: inp.port,
        input: {
          get source() {
            return pipelineAsSource(pipelineOf(inp.nodeId));
          },
          withStages(stages: readonly string[]) {
            const p = pipelineOf(inp.nodeId);
            return pipelineAsSource({...p, stages: [...p.stages, ...stages]});
          },
        },
      })),
      node.inputColumns,
      undefined,
    );
    const frag: PsnFragment | GeneratorError | undefined = generate(
      node.node.config,
      ctx,
    );
    if (frag === undefined) return generatorError(node, 'incomplete node');
    if (isError(frag)) return fromGeneratorError(frag);
    for (const inc of node.includes.pipe) includes.add(inc);

    if (frag.source !== undefined) {
      states.set(node.nodeId, {source: frag.source, stages: frag.stages ?? []});
      continue;
    }
    // Stages continue the primary input's pipeline. Copying it repeats the
    // pipeline when it is used more than once.
    const primary = node.inputs[0]?.nodeId;
    if (primary === undefined) return generatorError(node, 'no input');
    const p = pipelineOf(primary);
    states.set(node.nodeId, {
      source: p.source,
      stages: [...p.stages, ...(frag.stages ?? [])],
    });
  }

  const root = states.get(spec.rootId);
  if (root === undefined) return {ok: false, error: 'Empty plan.'};
  return {
    ok: true,
    sql: renderPipeline(root),
    includes: [...includes],
    relation: bareTableOf(root),
  };
}

// --- Public API ---

export function compose(
  plan: Plan,
  spec: ComposeSpec,
  dialect: SqlDialect,
): FormatResult {
  return dialect === 'pipe' ? composePsn(plan, spec) : composeSql(plan, spec);
}

function withIncludeHeaders(res: FormatResult): FormatResult {
  if (!res.ok || res.includes.length === 0) return res;
  const headers = res.includes
    .map((inc) => `INCLUDE PERFETTO MODULE ${inc};`)
    .join('\n');
  return {...res, sql: `${headers}\n\n${res.sql}`, relation: undefined};
}

/** Formats the whole plan as a single query (no cache tables). */
export function formatPlan(
  plan: Plan,
  dialect: SqlDialect = 'sql',
  options?: FormatOptions,
): FormatResult {
  const res = compose(
    plan,
    {
      members: new Set(plan.nodes.map((n) => n.nodeId)),
      rootId: plan.targetNodeId,
      tableFor: (nodeId) => nodeId,
    },
    dialect,
  );
  return options?.includeModuleHeaders ? withIncludeHeaders(res) : res;
}

export function buildDisplayQuery(
  plan: Plan | undefined,
  dialect: SqlDialect = 'sql',
): string | undefined {
  if (plan === undefined || plan.nodes.length === 0) return undefined;
  const res = formatPlan(plan, dialect, {includeModuleHeaders: true});
  return res.ok ? res.sql : `-- ${res.error}`;
}
