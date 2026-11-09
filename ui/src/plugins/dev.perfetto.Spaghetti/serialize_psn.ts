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

// Serializes the graph upstream of an output node into one PerfettoSQL Next
// (PSN) query.
//
// Stage fragments are appended to their primary input's pipeline; source
// fragments start a new pipeline. PSN has no CTEs, so a pipeline read more than
// once is repeated.

import {ensureDefined} from '../../base/assert';
import {type GraphIndex, resolveInputs, tryGetManifest} from './graph_utils';
import type {PsnInput} from './node_types';
import {
  indent,
  makeContext,
  renderOrder,
  type SerializeOptions,
  type SerializeResult,
} from './serialize_common';

interface Pipeline {
  /** A full source clause, e.g. `FROM slice` or `INTERVAL INTERSECTION OF`. */
  readonly source: string;
  readonly stages: readonly string[];
  /** Set when the pipeline is just a table, read by this name. */
  readonly table?: string;
  /** Set when the pipeline is just a parenthesized plain SQL subquery. */
  readonly subquery?: string;
}

/** Serializes everything upstream of `outputId` as one PSN query. */
export function serializePsn(
  index: GraphIndex,
  outputId: string,
  options: SerializeOptions = {},
): SerializeResult {
  const tables = options.tables ?? new Map<string, string>();
  const order = renderOrder(index, outputId, tables);
  if (!Array.isArray(order)) return order;

  const pipelines = new Map<string, Pipeline>();
  const includes = new Set<string>();

  function pipelineOf(nodeId: string): Pipeline {
    const table = tables.get(nodeId);
    if (table !== undefined) {
      return {source: `FROM ${table}`, stages: [], table};
    }
    return ensureDefined(pipelines.get(nodeId));
  }

  for (const nodeId of order) {
    // Checked during the walk.
    const node = ensureDefined(index.nodes[nodeId]);
    const manifest = ensureDefined(tryGetManifest(node.type));
    const generate = manifest.generators.psn;
    if (generate === undefined) {
      return {
        ok: false,
        error: `${manifest.title}: not supported in PerfettoSQL Next mode`,
        nodeId,
      };
    }
    const inputs = resolveInputs(index, nodeId);
    const frag = generate(
      node.config,
      makeContext(
        index,
        inputs,
        (id): PsnInput => ({
          source: asSource(pipelineOf(id)),
          table: pipelineOf(id).table,
          withStages: (stages) => {
            const p = pipelineOf(id);
            return asSource({
              source: p.source,
              stages: [...p.stages, ...stages],
            });
          },
        }),
        options.sqlModules,
      ),
    );
    if (frag === undefined) {
      return {ok: false, error: `${manifest.title}: incomplete node`, nodeId};
    }
    if ('error' in frag) return {ok: false, error: frag.error, nodeId};
    for (const inc of frag.includes ?? []) includes.add(inc);

    // A new pipeline. Only one with no stages is still just a table or a
    // subquery.
    const ownStages = frag.stages ?? [];
    const bare = ownStages.length === 0;
    if (frag.table !== undefined) {
      pipelines.set(nodeId, {
        source: `FROM ${frag.table}`,
        stages: ownStages,
        table: bare ? frag.table : undefined,
      });
      continue;
    }
    if (frag.subquery !== undefined) {
      pipelines.set(nodeId, {
        source: `FROM ${frag.subquery}`,
        stages: ownStages,
        subquery: bare ? frag.subquery : undefined,
      });
      continue;
    }
    if (frag.source !== undefined) {
      pipelines.set(nodeId, {source: frag.source, stages: ownStages});
      continue;
    }
    // Stages continue the primary input's pipeline (an empty fragment passes
    // it through). Copying it repeats the pipeline when it is read twice.
    const primary = inputs[0]?.upstreamId;
    if (primary === undefined) {
      return {ok: false, error: `${manifest.title}: no input`, nodeId};
    }
    const p = pipelineOf(primary);
    const stages = frag.stages ?? [];
    pipelines.set(
      nodeId,
      stages.length === 0
        ? p
        : {source: p.source, stages: [...p.stages, ...stages]},
    );
  }

  const root = pipelineOf(outputId);
  return {
    ok: true,
    sql: renderPipeline(root),
    includes: [...includes],
    relation: root.table,
  };
}

function renderPipeline(p: Pipeline): string {
  return [p.source, ...p.stages.map((s) => `|> ${s}`)].join('\n');
}

/**
 * A pipeline as a source: a table name, a plain SQL subquery as is, or the
 * pipeline in parentheses.
 */
function asSource(p: Pipeline): string {
  return p.table ?? p.subquery ?? `(\n${indent(renderPipeline(p))}\n)`;
}
