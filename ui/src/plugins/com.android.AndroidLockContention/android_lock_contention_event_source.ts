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
import {
  EventSource,
  RelatedEvent,
  RelatedEventData,
  Relation,
  getTrackUriForTrackId,
} from '../dev.perfetto.RelatedEvents';
import {time, duration} from '../../base/time';
import {STR, NUM_NULL, LONG_NULL} from '../../trace_processor/query_result';
import {enrichDepths} from '../dev.perfetto.RelatedEvents/utils';

export type OnDataLoadedCallback = (data: RelatedEventData) => void;

export class AndroidLockContentionEventSource implements EventSource {
  private onDataLoadedCallback?: OnDataLoadedCallback;

  constructor(private trace: Trace) {}

  setOnDataLoadedCallback(callback: OnDataLoadedCallback) {
    this.onDataLoadedCallback = callback;
  }

  async getRelatedEventData(eventId: number): Promise<RelatedEventData> {
    const query = `
      SELECT
        amc.id AS contention_id,
        amc.ts AS contention_ts,
        amc.dur AS contention_dur,
        amc.track_id AS blocked_track_id,
        amc.blocked_utid,
        amc.blocking_utid,
        amc.short_blocked_method,
        amc.short_blocking_method,
        amc.blocking_thread_name,
        s.id AS blocking_slice_id,
        s.track_id AS blocking_track_id,
        s.ts AS blocking_ts,
        s.dur AS blocking_dur
      FROM android_monitor_contention amc
      LEFT JOIN thread_or_process_slice s ON s.utid = amc.blocking_utid
        AND amc.ts >= s.ts
        AND amc.ts < s.ts + s.dur
      WHERE amc.id = ${eventId}
      ORDER BY s.depth DESC, s.id DESC
      LIMIT 1
    `;

    const result = await this.trace.engine.query(query);
    const it = result.iter({
      contention_id: NUM_NULL,
      contention_ts: LONG_NULL,
      contention_dur: LONG_NULL,
      blocked_track_id: NUM_NULL,
      blocked_utid: NUM_NULL,
      blocking_utid: NUM_NULL,
      short_blocked_method: STR,
      short_blocking_method: STR,
      blocking_thread_name: STR,
      blocking_slice_id: NUM_NULL,
      blocking_track_id: NUM_NULL,
      blocking_ts: LONG_NULL,
      blocking_dur: LONG_NULL,
    });

    const events: RelatedEvent[] = [];
    const overlayEvents: RelatedEvent[] = [];
    const overlayRelations: Relation[] = [];

    if (!it.valid()) {
      const data = {events: [], relations: [], overlayEvents, overlayRelations};
      this.onDataLoadedCallback?.(data);
      return data;
    }

    const blockedEventId = it.contention_id!;
    const blockedTs = BigInt(it.contention_ts!) as time;
    const blockedDur = BigInt(it.contention_dur!) as duration;
    const blockedTrackId = it.blocked_track_id!;
    const blockingThreadName = it.blocking_thread_name;

    const blockedTrackUri = getTrackUriForTrackId(this.trace, blockedTrackId);
    const blockingTrackId = it.blocking_track_id;
    const blockingTrackUri =
      typeof blockingTrackId === 'number'
        ? getTrackUriForTrackId(this.trace, blockingTrackId)
        : undefined;

    const tabEvent: RelatedEvent = {
      id: blockedEventId,
      ts: blockedTs,
      dur: blockedDur,
      trackUri: blockedTrackUri,
      type: 'Lock Contention',
      customArgs: {
        short_blocked_method: it.short_blocked_method,
        short_blocking_method: it.short_blocking_method,
        blocking_thread_name: blockingThreadName,
        blockingTrackUri: blockingTrackUri,
        blockingSliceId: it.blocking_slice_id,
      },
    };
    events.push(tabEvent);

    // Event for the selected contention slice itself
    const blockedOverlayEvent: RelatedEvent = {
      id: blockedEventId,
      ts: blockedTs,
      dur: blockedDur,
      trackUri: blockedTrackUri,
      type: 'Lock Contention',
      customArgs: {
        Blocked: it.short_blocked_method,
        Blocking: it.short_blocking_method,
      },
    };
    overlayEvents.push(blockedOverlayEvent);

    const blockingSliceId = it.blocking_slice_id;
    if (blockingSliceId !== null && blockingTrackUri) {
      const blockingEvent: RelatedEvent = {
        id: blockingSliceId,
        ts: BigInt(it.blocking_ts!) as time,
        dur: BigInt(it.blocking_dur!) as duration,
        trackUri: blockingTrackUri,
        type: 'Blocking Slice',
        customArgs: {
          name: it.short_blocking_method,
        },
      };
      overlayEvents.push(blockingEvent);

      const relation: Relation = {
        sourceId: blockedEventId,
        targetId: blockingSliceId,
        type: 'blocked_on',
        customArgs: {color: 'red'},
      };
      overlayRelations.push(relation);
    }

    await enrichDepths(this.trace, overlayEvents);

    const data = {events, relations: [], overlayEvents, overlayRelations};
    this.onDataLoadedCallback?.(data);
    return data;
  }
}
