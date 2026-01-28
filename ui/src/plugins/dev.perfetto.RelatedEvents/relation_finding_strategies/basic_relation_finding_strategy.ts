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

import {
  Dataset,
  SourceDataset,
  UnionDatasetWithLineage,
} from '../../../trace_processor/dataset';
import {
  EventContext,
  RELATED_EVENT_SCHEMA,
  RelationFindingStrategy,
  RelationRule,
} from '../relation_finding_strategy';
import {LONG, NUM, STR, STR_NULL} from '../../../trace_processor/query_result';
import {Time, time, duration} from '../../../base/time';
import {Trace} from '../../../public/trace';

interface DetailedEventInfo {
  eventName: string;
  eventTs: time;
  eventDur: duration;
  trackId: number;
  eventArgs: Map<string, string>;
}

export class BasicRelationFindingStrategy implements RelationFindingStrategy {
  constructor(private rules: RelationRule[]) {}

  async findRelatedEvents(trace: Trace): Promise<Dataset | undefined> {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') {
      return undefined;
    }

    const initialEventId = Number(selection.eventId);
    if (isNaN(initialEventId)) {
      return undefined;
    }

    const initialTrack = trace.tracks.getTrack(selection.trackUri);
    if (!initialTrack) {
      return undefined;
    }

    const initialDataset = initialTrack.renderer.getDataset?.();
    if (!initialDataset) return undefined;

    const detailsMap = await this.getEventDetails(
      trace,
      initialEventId,
      initialDataset,
    );
    const initialEventDetails = detailsMap.get(initialEventId);

    if (!initialEventDetails) {
      return undefined;
    }

    const context: EventContext = {
      sliceId: initialEventId,
      name: initialEventDetails.eventName,
      args: initialEventDetails.eventArgs,
      ts: initialEventDetails.eventTs,
      dur: initialEventDetails.eventDur,
    };

    const relatedDatasets: Dataset[] = this.rules.flatMap((rule) =>
      rule.getRelatedEventsAsDataset(context),
    );

    relatedDatasets.push(
      new SourceDataset({
        ...initialDataset,
        filter: {col: 'id', eq: initialEventId},
      }),
    );

    if (relatedDatasets.length === 0) return undefined;

    const unionDataset = UnionDatasetWithLineage.create(relatedDatasets);

    return unionDataset;
  }

  private async getEventDetails(
    trace: Trace,
    sliceId: number,
    dataset: Dataset,
  ): Promise<Map<number, DetailedEventInfo>> {
    const trackBaseQuery = dataset.query({
      ...RELATED_EVENT_SCHEMA,
      arg_set_id: NUM,
    });
    const sql = `
        SELECT
            b.id,
            b.name,
            b.ts, 
            b.dur,
            b.track_id,
            args.key AS arg_key, 
            args.display_value AS arg_value
        FROM (${trackBaseQuery}) b
        LEFT JOIN args ON b.arg_set_id = args.arg_set_id
        WHERE b.id = ${sliceId}
        `;

    try {
      const result = await trace.engine.query(sql);
      if (result.numRows() === 0) return new Map();

      const it = result.iter({
        id: NUM,
        name: STR,
        ts: LONG,
        dur: LONG,
        track_id: NUM,
        arg_key: STR_NULL,
        arg_value: STR_NULL,
      });
      const eventsMap = new Map<number, DetailedEventInfo>();

      while (it.valid()) {
        const id = it.id;
        let info = eventsMap.get(id);
        if (!info) {
          info = {
            eventName: it.name,
            eventTs: Time.fromRaw(it.ts),
            eventDur: Time.fromRaw(it.dur),
            trackId: it.track_id,
            eventArgs: new Map<string, string>(),
          };
          eventsMap.set(id, info);
        }

        if (it.arg_key && it.arg_value) {
          info.eventArgs.set(it.arg_key, it.arg_value);
        }
        it.next();
      }
      return eventsMap;
    } catch (e) {
      return new Map();
    }
  }
}
