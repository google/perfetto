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

import type {Trace} from '../../public/trace';
import {STR, NUM, LONG, UNKNOWN} from '../../trace_processor/query_result';
import {
  type Dataset,
  UnionDatasetWithLineage,
} from '../../trace_processor/dataset';

const RELATION_SCHEMA = {
  id: NUM,
  name: STR,
  ts: LONG,
  dur: LONG,
  track_id: NUM,
  depth: NUM,
};

export function getTrackUriForTrackId(trace: Trace, trackId: number): string {
  const track = trace.tracks.findTrack((t) =>
    t.tags?.trackIds?.includes(trackId),
  );
  return track?.uri || `/slice_${trackId}`;
}

interface DepthTarget {
  id: number;
  trackUri: string;
  depth: number;
}

// Enriches a list of targets with their depth within their respective tracks.
export async function enrichDepths(
  trace: Trace,
  targets: ReadonlyArray<DepthTarget>,
): Promise<void> {
  const trackUris = new Set<string>();
  const eventIds = new Set<number>();
  const nodeMap = new Map<number, DepthTarget[]>();

  for (const target of targets) {
    if (target.trackUri) {
      trackUris.add(target.trackUri);
      eventIds.add(target.id);
      if (!nodeMap.has(target.id)) nodeMap.set(target.id, []);
      nodeMap.get(target.id)!.push(target);
    }
  }

  if (eventIds.size === 0) return;

  const trackDatasets: Dataset[] = [];
  for (const trackUri of trackUris) {
    const track = trace.tracks.getTrack(trackUri);
    if (track?.renderer?.getDataset) {
      const ds = track.renderer.getDataset();
      if (ds) trackDatasets.push(ds);
    }
  }

  if (trackDatasets.length === 0) return;

  const unionDataset = UnionDatasetWithLineage.create(trackDatasets);
  const idsArray = Array.from(eventIds);
  const querySchema = {
    ...RELATION_SCHEMA,
    __groupid: NUM,
    __partition: UNKNOWN,
  };

  const sql = `SELECT * FROM (${unionDataset.query(querySchema)}) WHERE id IN (${idsArray.join(',')})`;

  try {
    const result = await trace.engine.query(sql);
    const it = result.iter(querySchema);
    while (it.valid()) {
      const nodes = nodeMap.get(it.id);
      if (nodes) {
        nodes.forEach((n) => (n.depth = Number(it.depth)));
      }
      it.next();
    }
  } catch (e) {
    console.error(`Error fetching depths:`, e);
  }
}

export class TrackPinningManager {
  // Tracks that were pinned by this manager. We only unpin tracks that are in this set
  // to avoid overriding manual pins or pins by other plugins.
  private managedPinnedTrackUris = new Set<string>();

  constructor(private readonly trace: Trace) {}

  pinTracks(uris: ReadonlyArray<string>) {
    uris.forEach((uri) => {
      const trackNode = this.trace.currentWorkspace.getTrackByUri(uri);
      if (trackNode) {
        if (!trackNode.isPinned) {
          trackNode.pin();
          this.managedPinnedTrackUris.add(uri);
        }
      }
    });
  }

  unpinTracks(uris: ReadonlyArray<string>) {
    uris.forEach((uri) => {
      if (this.managedPinnedTrackUris.has(uri)) {
        const trackNode = this.trace.currentWorkspace.getTrackByUri(uri);
        if (trackNode && trackNode.isPinned) {
          trackNode.unpin();
        }
        this.managedPinnedTrackUris.delete(uri);
      }
    });
  }

  isTrackPinned(uri: string): boolean {
    const trackNode = this.trace.currentWorkspace.getTrackByUri(uri);
    const isTimelinePinned = trackNode?.isPinned ?? false;

    // If the track was unpinned manually on the timeline, forget that we managed it.
    // This prevents accidentally unpinning it later if the user pins it again manually.
    if (!isTimelinePinned && this.managedPinnedTrackUris.has(uri)) {
      this.managedPinnedTrackUris.delete(uri);
    }

    return isTimelinePinned;
  }

  getPinnedUris(): ReadonlySet<string> {
    const uris = new Set<string>();
    this.trace.currentWorkspace.flatTracks.forEach((t) => {
      if (t.uri && t.isPinned) {
        uris.add(t.uri);
      }
    });
    return uris;
  }
}
