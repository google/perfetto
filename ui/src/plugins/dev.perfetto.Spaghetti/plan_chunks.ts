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

// Splits the graph upstream of a target node into chunks: the units that each
// become one query. Every expensive node (its manifest is marked `expensive`)
// roots a chunk, and the target roots the output chunk. Every other node joins
// the chunk of each expensive node (or the target) downstream of it, so a
// cheap node read from several chunks is repeated in each of them.
//
// Pure and structural: no SQL, no dialect, no cache. Chunks only point at node
// ids in the graph. Turning them into queries is up to the serializer, and
// deciding which chunks to materialize is up to the materializer.

import {type GraphIndex, resolveInputs, tryGetManifest} from './graph_utils';

export interface PlanChunk {
  /** The node whose output this chunk produces. */
  readonly rootId: string;
  /** Nodes rendered in this chunk's query, in topological order (root last). */
  readonly nodeIds: readonly string[];
  /** Root ids of the chunks this chunk reads from. */
  readonly inputs: readonly string[];
}

export interface ChunkPlan {
  /** In dependency order: every chunk comes after the chunks it reads. */
  readonly chunks: readonly PlanChunk[];
  /**
   * Root id of the chunk that produces the target's result. It's the last
   * chunk.
   */
  readonly outputId: string;
}

/**
 * Chunks the graph upstream of `targetId`. Returns undefined if the target or
 * anything upstream of it has an unknown node type, or if there's a cycle.
 */
export function planChunks(
  index: GraphIndex,
  targetId: string,
): ChunkPlan | undefined {
  // Resolve every reachable node's inputs once, checking types and cycles.
  const inputsOf = new Map<string, readonly string[]>();
  const expensive = new Set<string>();
  const visiting = new Set<string>();

  function resolve(nodeId: string): boolean {
    if (inputsOf.has(nodeId)) return true;
    if (visiting.has(nodeId)) return false; // cycle
    const node = index.nodes[nodeId];
    const manifest = node && tryGetManifest(node.type);
    if (node === undefined || manifest === undefined) return false;

    visiting.add(nodeId);
    const inputs = resolveInputs(index, nodeId).map((i) => i.upstreamId);
    if (!inputs.every(resolve)) return false;
    visiting.delete(nodeId);

    inputsOf.set(nodeId, inputs);
    if (manifest.expensive) expensive.add(nodeId);
    return true;
  }
  if (!resolve(targetId)) return undefined;

  // One chunk per expensive node (memoized, so each is built once), plus the
  // output chunk rooted at the target.
  const chunks = new Map<string, PlanChunk>();

  function chunkFor(rootId: string): void {
    if (chunks.has(rootId)) return;
    const nodeIds: string[] = [];
    const inputs = new Set<string>();
    const seen = new Set<string>();
    function collect(nodeId: string): void {
      if (seen.has(nodeId)) return;
      seen.add(nodeId);
      for (const inp of inputsOf.get(nodeId) ?? []) {
        if (expensive.has(inp)) {
          inputs.add(inp);
        } else {
          collect(inp);
        }
      }
      nodeIds.push(nodeId); // post-order: inputs first
    }
    collect(rootId);

    inputs.forEach(chunkFor); // dependencies are added first
    chunks.set(rootId, {
      rootId,
      nodeIds,
      inputs: [...inputs],
    });
  }
  chunkFor(targetId);

  return {chunks: [...chunks.values()], outputId: targetId};
}
