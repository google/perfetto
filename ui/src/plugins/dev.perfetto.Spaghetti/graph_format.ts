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

import {z} from 'zod';
import {NODE_REGISTRY} from './graph_utils';
import type {NodeManifest} from './node_types';

function isRecord(value: unknown): value is Record<string, unknown> {
  return value !== null && typeof value === 'object' && !Array.isArray(value);
}

/**
 * Formats a JSON Schema (produced by `z.toJSONSchema(manifest.schema)`) into a
 * compact TypeScript-style type notation for LLM consumption, preserving field
 * optionality, literal unions, nested objects, and `.describe(...)` comments
 * without verbose JSON Schema boilerplate.
 */
export function formatJsonSchemaAsType(
  schema: Record<string, unknown>,
  indent = '  ',
): string {
  if (Array.isArray(schema.enum)) {
    return schema.enum.map((v) => JSON.stringify(v)).join(' | ');
  }
  if ('const' in schema) {
    return JSON.stringify(schema.const);
  }
  const unionVariants = Array.isArray(schema.anyOf)
    ? schema.anyOf
    : Array.isArray(schema.oneOf)
      ? schema.oneOf
      : undefined;
  if (unionVariants !== undefined) {
    return unionVariants
      .map((v) => (isRecord(v) ? formatJsonSchemaAsType(v, indent) : 'unknown'))
      .join(' | ');
  }

  if (schema.type === 'array') {
    const itemSchema = isRecord(schema.items) ? schema.items : undefined;
    if (!itemSchema) return 'unknown[]';
    const itemType = formatJsonSchemaAsType(itemSchema, indent);
    return itemType.includes('\n') || itemType.includes(' | ')
      ? `Array<${itemType}>`
      : `${itemType}[]`;
  }

  if (schema.type === 'object') {
    const props = isRecord(schema.properties) ? schema.properties : {};
    const keys = Object.keys(props);
    if (keys.length === 0) return '{}';
    const required = new Set(
      Array.isArray(schema.required)
        ? schema.required.filter((r): r is string => typeof r === 'string')
        : [],
    );
    const lines = keys.map((key) => {
      const propSchema = props[key];
      const opt = required.has(key) ? '' : '?';
      const propType = isRecord(propSchema)
        ? formatJsonSchemaAsType(propSchema, `${indent}  `)
        : 'unknown';
      const desc =
        isRecord(propSchema) && typeof propSchema.description === 'string'
          ? ` // ${propSchema.description}`
          : '';
      return `${indent}  ${key}${opt}: ${propType};${desc}`;
    });
    return `{\n${lines.join('\n')}\n${indent}}`;
  }

  if (schema.type === 'integer') return 'number';
  if (typeof schema.type === 'string') return schema.type;
  return 'unknown';
}

function formatManifestPorts(manifest: NodeManifest): string {
  if (!manifest.getInputs) {
    return 'none (source node; root only, cannot be docked in "next")';
  }
  const defaultCfg = manifest.defaultConfig() as Record<string, unknown>;
  if (typeof defaultCfg.numInputs === 'number') {
    return `[0..numInputs-1] (default numInputs=${defaultCfg.numInputs})`;
  }
  if (Array.isArray(defaultCfg.inputPorts)) {
    return '[0..inputPorts.length-1] (named by config.inputPorts)';
  }
  const ports = manifest.getInputs(defaultCfg);
  return `[${ports.map((p, idx) => `${idx}: ${p.name}${p.optional ? ' (optional)' : ''}`).join(', ')}]`;
}

export function buildNodeCatalogFromRegistry(
  registry: Readonly<Record<string, NodeManifest>> = NODE_REGISTRY,
): string {
  return Object.entries(registry)
    .map(([type, manifest]) => {
      const jsonSchema = z.toJSONSchema(manifest.schema, {
        target: 'draft-2020-12',
      }) as Record<string, unknown>;
      const summary =
        typeof jsonSchema.description === 'string'
          ? ` — ${jsonSchema.description}`
          : '';
      const configShape = formatJsonSchemaAsType(jsonSchema, '  ');
      return [
        `- "${type}" (${manifest.title})${summary}`,
        `  ports: ${formatManifestPorts(manifest)}`,
        `  config: ${configShape}`,
      ].join('\n');
    })
    .join('\n\n');
}

const GRAPH_ENVELOPE_HEADER = `
================================================================================
SPAGHETTI QUERY BUILDER GRAPH JSON FORMAT
================================================================================

Send the whole graph as a JSON string. Top-level shape:

{
  "nodes": [ <rootNode>, ... ],
  "labels": []
}

Root nodes live on the canvas ("nodes" array):
{
  "id": "<unique string id, e.g. \\"n1\\">",
  "type": "<node type from catalog below>",
  "x": 80,                                  // OPTIONAL on write (auto-laid out if omitted)
  "y": 80,                                  // OPTIONAL on write (auto-laid out if omitted)
  "config": { ...type-specific fields... },
  "inputs": [ "<upstreamId>" | null, ... ], // OPTIONAL: wired input ports [port0, port1, ...]
  "next": <dockedNode>                      // OPTIONAL: single node docked directly below
}

Docked nodes ("next") nest linearly inside their parent and OMIT "x" and "y":
{
  "id": "<unique string id>",
  "type": "<node type with >= 1 input port>",
  "config": { ... },
  "inputs": [ null, "<sideUpstreamId>" ],   // OPTIONAL: side inputs on port 1, 2, ...
  "next": <dockedNode>
}

--------------------------------------------------------------------------------
CONNECTION MODEL & GUIDELINES
--------------------------------------------------------------------------------
1. DOCKED CHAINS ("next") - STRONGLY PREFERRED for linear steps:
   Placing node B in node A's "next" automatically feeds port 0 of B (stacked as
   one vertical pipeline card). Do NOT also put A's id in B's "inputs[0]".
   Source nodes (ports: none) must be root nodes and cannot be docked in "next".

2. WIRED CONNECTIONS ("inputs") - for multi-input nodes & branching:
   Positional array [port0, port1, ...]:
   - Standalone root multi-input node: "inputs": ["leftTailId", "rightTailId"]
   - Multi-input node docked under its port-0 branch via "next": slot 0 comes
     from docking, so wire side ports as "inputs": [null, "rightTailId"]
   - Always reference the specific tail node id whose output you want. No cycles.

3. PREFER STRUCTURED NODES OVER "sql":
   Decompose pipelines into structured nodes ("from" -> "filter" -> "groupby" ->
   "sort" -> "limit", "join", etc.) rather than packing raw SQL into "sql".
   Confirm table/column names with get_schema / run_query first, and call
   get_spaghetti_graph before editing an existing graph.

4. LAYOUT (only matters if you give "x"/"y"; omit them to auto-layout):
   Data flows left to right: input ports are on a node's left, its output on
   the right. Place each root to the RIGHT of the roots it reads from, and
   stack parallel branches vertically. Docked "next" nodes stack below their
   parent automatically. Rough rendered sizes (actual size depends on config):
   - Width: ~200-300px per node (200px minimum).
   - Height: ~40px header + ~30-40px per config row; a docked chain is the sum
     of its nodes. Most simple nodes are ~80-150px tall.
   Leave ~80px horizontally between columns and ~80px vertically between
   stacked roots, e.g. columns at x = 80, 420, 760, ...

================================================================================
NODE CATALOG (auto-generated from each node's Zod schema)
================================================================================
`.trim();

export const SPAGHETTI_GRAPH_FORMAT = `${GRAPH_ENVELOPE_HEADER}\n\n${buildNodeCatalogFromRegistry()}`;
