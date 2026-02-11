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

import {Trace} from '../../public/trace';
import {RelatedEvent} from './interface';
import {STR, NUM, LONG, UNKNOWN} from '../../trace_processor/query_result';
import {Dataset, UnionDatasetWithLineage} from '../../trace_processor/dataset';

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

export async function enrichDepths(
  trace: Trace,
  events: RelatedEvent[],
): Promise<void> {
  const trackUris = new Set<string>();
  const eventIds = new Set<number>();
  const nodeMap = new Map<number, RelatedEvent[]>();

  for (const event of events) {
    if (event.trackUri) {
      trackUris.add(event.trackUri);
      eventIds.add(event.id);
      if (!nodeMap.has(event.id)) nodeMap.set(event.id, []);
      nodeMap.get(event.id)!.push(event);
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
  private pinnedTrackUris = new Set<string>();

  pinTracks(uris: string[]) {
    uris.forEach((uri) => this.pinnedTrackUris.add(uri));
  }

  unpinTracks(uris: string[]) {
    uris.forEach((uri) => this.pinnedTrackUris.delete(uri));
  }

  isTrackPinned(uri: string): boolean {
    return this.pinnedTrackUris.has(uri);
  }

  applyPinning(trace: Trace) {
    trace.currentWorkspace.flatTracks.forEach((trackNode) => {
      if (!trackNode.uri) return;

      const shouldBePinned = this.pinnedTrackUris.has(trackNode.uri);
      if (shouldBePinned && !trackNode.isPinned) {
        trackNode.pin();
      } else if (!shouldBePinned && trackNode.isPinned) {
        // TODO(ivankc) Consider tracks pinned by other means
        trackNode.unpin();
      }
    });
  }

  getPinnedUris(): ReadonlySet<string> {
    return this.pinnedTrackUris;
  }
}
