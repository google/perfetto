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

import {ArrowConnection, ArrowVisualiser} from './arrow_visualiser';
import {Dataset} from '../../../trace_processor/dataset';
import {RELATION_SCHEMA} from '../relation_finding_strategy';
import {time, Time} from '../../../base/time';
import {TimeScale} from '../../../base/time_scale';
import {Trace} from '../../../public/trace';
import {TrackBounds} from '../../../public/track';

function getTrackUriForTrackId(trace: Trace, trackId: number): string {
  const track = trace.tracks.findTrack((t) =>
    t.tags?.trackIds?.includes(trackId),
  );
  if (track?.uri) {
    return track.uri;
  }
  return `/slice_${trackId}`;
}
interface EventInfo {
  id: number;
  ts: time;
  dur: time;
  track_id: number;
  depth: number;
}

/**
 * Draws relations between events in a dataset, assuming they should be
 * connected in timestamp order.
 */
export class RelationVisualiser {
  private visualiser: ArrowVisualiser;
  private relations: ArrowConnection[] = [];
  private currentDataset: Dataset | undefined;

  constructor(private trace: Trace) {
    this.visualiser = new ArrowVisualiser(trace);
  }

  /**
   * Calculates relations from the dataset and draws relation arrows on the canvas.
   * @param canvasCtx Canvas rendering context to draw on.
   * @param timescale Timescale for time to pixel conversion.
   * @param renderedTracks Information about visible tracks bounds.
   * @param dataset A dataset containing related events. MUST implement RELATION_SCHEMA.
   */
  async drawRelations(
    canvasCtx: CanvasRenderingContext2D,
    timescale: TimeScale,
    renderedTracks: ReadonlyArray<TrackBounds>,
    dataset: Dataset,
  ) {
    if (dataset !== this.currentDataset) {
      this.currentDataset = dataset;
      this.relations = [];
      this.loadData(dataset);
    }

    if (this.relations.length > 0) {
      this.visualiser.draw(
        canvasCtx,
        timescale,
        renderedTracks,
        this.relations,
      );
    }
  }

  private async loadData(dataset: Dataset) {
    const events = await this.getAllEvents(dataset);

    // If the dataset has changed while we were fetching, discard the results
    if (dataset !== this.currentDataset) return;

    if (events.length < 2) {
      this.relations = [];
      return;
    }

    events.sort((a, b) => Number(a.ts - b.ts));

    this.relations = this.buildRelations(events);
  }

  private async getAllEvents(dataset: Dataset): Promise<EventInfo[]> {
    if (!dataset.implements(RELATION_SCHEMA)) {
      return [];
    }

    const events: EventInfo[] = [];
    const sql = dataset.query(RELATION_SCHEMA);

    try {
      const result = await this.trace.engine.query(sql);
      const it = result.iter(RELATION_SCHEMA);
      for (; it.valid(); it.next()) {
        events.push({
          id: Number(it.id),
          ts: Time.fromRaw(it.ts),
          dur: Time.fromRaw(it.dur),
          track_id: Number(it.track_id),
          depth: Number(it.depth),
        });
      }
    } catch (e) {
      console.error('RelationVisualiser: Error fetching events:', e);
    }
    return events;
  }

  private buildRelations(events: EventInfo[]): ArrowConnection[] {
    const relations: ArrowConnection[] = [];
    for (let i = 0; i < events.length - 1; i++) {
      const sourceEvent = events[i];
      const destEvent = events[i + 1];

      const startTs = Time.add(sourceEvent.ts, sourceEvent.dur);
      const endTs = destEvent.ts;

      relations.push({
        start: {
          trackUri: getTrackUriForTrackId(this.trace, sourceEvent.track_id),
          ts: startTs,
          depth: sourceEvent.depth,
        },
        end: {
          trackUri: getTrackUriForTrackId(this.trace, destEvent.track_id),
          ts: endTs,
          depth: destEvent.depth,
        },
      });
    }
    return relations;
  }
}
