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

import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import type {TrackNode} from '../../public/workspace';
import {
  type Dimension,
  dimensionValue,
  formatDimensionLabels,
  visibleDimensionNames,
} from '../../public/dimensions';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';

// The subtitle item id used for dimension labels. Distinct from the ids used by
// other contributors (e.g. Chrome process labels) so the two compose instead of
// overwriting each other.
const SUBTITLE_ITEM_ID = 'dimensions';

// A custom dimension of a trace processor track.
interface TrackDimension {
  readonly trackId: number;
  // The upid of the process if the track is the root track of a process
  // descriptor, i.e. the process track.
  readonly processUpid: number | null;
  readonly dimension: Dimension;
}

/**
 * Surfaces track dimensions in the timeline.
 *
 * Dimensions are a trace processor concept first: this plugin implements the
 * shared presentation half of it, i.e. the collapse rule (see
 * visibleDimensionNames) and the subtitle labels which come out of it. Well
 * known dimensions which already have specialized presentation (GPU hierarchy,
 * machine name suffixes) are skipped: see public/dimensions.ts.
 */
export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.TrackDimensions';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    // Wait for the trace to be ready: every plugin has had a chance to build
    // its part of the workspace by then, which is what we annotate.
    ctx.onTraceReady.addListener(async () => {
      const trackDims = await queryDimensions(ctx);
      const visible = visibleDimensionNames(trackDims.map((d) => d.dimension));
      if (visible.size === 0) return;

      const byNode = new Map<TrackNode, Dimension[]>();
      const add = (node: TrackNode | undefined, dimension: Dimension) => {
        if (node === undefined) return;
        if (!visible.has(dimension.name)) return;
        const dims = byNode.get(node);
        if (dims === undefined) {
          byNode.set(node, [dimension]);
        } else if (!dims.some((d) => d.name === dimension.name)) {
          dims.push(dimension);
        }
      };

      // Trace processor tracks are mapped back to workspace nodes through the
      // tracks' `trackIds` tag. The dimensions of a process are the ones of its
      // process track, so they also label the process group itself rather than
      // only the process track, which is one of its children (or not shown at
      // all if it has no events).
      const nodesByTrackId = getNodesByTrackId(ctx);
      const processGroups = ctx.plugins.getPlugin(ProcessThreadGroupsPlugin);
      for (const {trackId, processUpid, dimension} of trackDims) {
        add(nodesByTrackId.get(trackId), dimension);
        if (processUpid !== null) {
          add(processGroups.getGroupForProcess(processUpid), dimension);
        }
      }

      for (const [node, dims] of byNode) {
        // A dimension is inherited by everything below the node which declared
        // it, so repeating the same label on every descendant is just noise:
        // only label a node when it says something its ancestors don't.
        const newDims = dims
          .filter((d) => !isShownByAncestor(node, d, byNode))
          .sort((a, b) => a.name.localeCompare(b.name));
        node.setSubtitleItem(SUBTITLE_ITEM_ID, formatDimensionLabels(newDims));
      }
    });
  }
}

function isShownByAncestor(
  node: TrackNode,
  dimension: Dimension,
  byNode: ReadonlyMap<TrackNode, ReadonlyArray<Dimension>>,
): boolean {
  const value = dimensionValue(dimension);
  for (let n = node.parent; n !== undefined; n = n.parent) {
    const dims = byNode.get(n);
    if (
      dims?.some(
        (d) => d.name === dimension.name && dimensionValue(d) === value,
      )
    ) {
      return true;
    }
  }
  return false;
}

function getNodesByTrackId(ctx: Trace): Map<number, TrackNode> {
  const result = new Map<number, TrackNode>();
  for (const track of ctx.tracks.getAllTracks()) {
    const node = ctx.defaultWorkspace.getTrackByUri(track.uri);
    if (node === undefined) continue;
    for (const trackId of track.tags?.trackIds ?? []) {
      result.set(trackId, node);
    }
  }
  return result;
}

// Note: this deliberately asks only for custom dimensions. Every well known
// dimension is currently presented elsewhere in the UI (see the list in
// public/dimensions.ts), and skipping them means traces without custom
// dimensions - i.e. almost all of them today - do no work here at all. Drop
// the filter when a well known dimension starts being labelled by this pass.
async function queryDimensions(
  ctx: Trace,
): Promise<ReadonlyArray<TrackDimension>> {
  // The root track of a process descriptor is the only track event track with
  // a process which is the root of its scope.
  const res = await ctx.engine.query(`
    SELECT
      d.track_id AS trackId,
      iif(
        extract_arg(t.source_arg_set_id, 'is_root_in_scope'),
        extract_arg(t.dimension_arg_set_id, 'upid'),
        NULL
      ) AS processUpid,
      d.name,
      d.int_value AS intValue,
      d.string_value AS stringValue,
      d.display_name AS displayName
    FROM track_dimension AS d
    JOIN track AS t ON t.id = d.track_id
    WHERE NOT d.is_well_known
    ORDER BY d.track_id, d.name
  `);
  const it = res.iter({
    trackId: NUM,
    processUpid: NUM_NULL,
    name: STR,
    intValue: NUM_NULL,
    stringValue: STR_NULL,
    displayName: STR_NULL,
  });
  const out: TrackDimension[] = [];
  for (; it.valid(); it.next()) {
    out.push({
      trackId: it.trackId,
      processUpid: it.processUpid,
      dimension: {
        name: it.name,
        intValue: it.intValue,
        stringValue: it.stringValue,
        displayName: it.displayName,
      },
    });
  }
  return out;
}
