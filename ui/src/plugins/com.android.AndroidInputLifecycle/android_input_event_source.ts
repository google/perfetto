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
  RelatedEventData,
  RelatedEvent,
  Relation,
  NavTarget,
} from '../../components/related_events/interface';
import {
  LONG_NULL,
  NUM_NULL,
  STR_NULL,
} from '../../trace_processor/query_result';
import {time, duration, Time} from '../../base/time';
import {getTrackUriForTrackId} from '../../components/related_events/utils';

export class AndroidInputEventSource {
  constructor(private trace: Trace) {}

  async getRelatedEventData(eventId: number): Promise<RelatedEventData> {
    const result = await this.trace.engine.query(
      `SELECT * FROM _android_input_lifecycle_by_slice_id(${eventId})`,
    );

    const events: RelatedEvent[] = [];
    const relations: Relation[] = [];

    const it = result.iter({
      input_id: STR_NULL,
      channel: STR_NULL,
      total_latency: LONG_NULL,

      ts_reader: LONG_NULL,
      id_reader: NUM_NULL,
      track_reader: NUM_NULL,
      dur_reader: LONG_NULL,

      ts_dispatch: LONG_NULL,
      id_dispatch: NUM_NULL,
      track_dispatch: NUM_NULL,
      dur_dispatch: LONG_NULL,

      ts_receive: LONG_NULL,
      id_receive: NUM_NULL,
      track_receive: NUM_NULL,
      dur_receive: LONG_NULL,

      ts_consume: LONG_NULL,
      id_consume: NUM_NULL,
      track_consume: NUM_NULL,
      dur_consume: LONG_NULL,

      ts_frame: LONG_NULL,
      id_frame: NUM_NULL,
      track_frame: NUM_NULL,
      dur_frame: LONG_NULL,
    });
    if (!it.valid()) {
      return {events: [], relations: []};
    }

    while (it.valid()) {
      const channel = it.channel;
      const totalLatency = it.total_latency !== null ? it.total_latency : null;

      const stages: (RelatedEvent | undefined)[] = [
        this.parseStage(
          'InputReader',
          channel,
          totalLatency,
          it.id_reader,
          it.ts_reader,
          it.dur_reader,
          it.track_reader,
        ),
        this.parseStage(
          'InputDispatcher',
          channel,
          totalLatency,
          it.id_dispatch,
          it.ts_dispatch,
          it.dur_dispatch,
          it.track_dispatch,
        ),
        this.parseStage(
          'AppReceive',
          channel,
          totalLatency,
          it.id_receive,
          it.ts_receive,
          it.dur_receive,
          it.track_receive,
        ),
        this.parseStage(
          'AppConsume',
          channel,
          totalLatency,
          it.id_consume,
          it.ts_consume,
          it.dur_consume,
          it.track_consume,
        ),
        this.parseStage(
          'AppFrame',
          channel,
          totalLatency,
          it.id_frame,
          it.ts_frame,
          it.dur_frame,
          it.track_frame,
        ),
      ];

      const [
        readerEvent,
        dispatchEvent,
        receiveEvent,
        consumeEvent,
        frameEvent,
      ] = stages;

      const tabEvent: RelatedEvent = {
        id: eventId,
        ts: (readerEvent?.ts ??
          dispatchEvent?.ts ??
          receiveEvent?.ts ??
          0n) as time,
        dur: totalLatency ?? 0n,
        trackUri: '',
        type: 'InputLifecycle',
        customArgs: {
          channel,
          totalLatency,
          reader: this.createStageArgs(readerEvent),
          dispatcher: this.createStageArgs(dispatchEvent, readerEvent),
          receiver: this.createStageArgs(receiveEvent, dispatchEvent),
          consumer: this.createStageArgs(consumeEvent, receiveEvent),
          frame: this.createStageArgs(frameEvent, consumeEvent),
          allTrackIds: [
            it.track_reader,
            it.track_dispatch,
            it.track_receive,
            it.track_consume,
            it.track_frame,
          ].filter((t) => t !== null) as number[],
        },
      };

      events.push(tabEvent);
      it.next();
    }

    return {events, relations};
  }

  private parseStage(
    type: string,
    channel: string | null,
    totalLatency: duration | null,
    id: number | null,
    ts: bigint | null,
    dur: bigint | null,
    trackId: number | null,
  ): RelatedEvent | undefined {
    if (id === null || ts === null || dur === null || trackId === null) {
      return undefined;
    }

    const trackUri = getTrackUriForTrackId(this.trace, trackId);
    if (!trackUri) return undefined;

    return {
      id,
      ts: Time.fromRaw(ts),
      dur,
      trackUri,
      type,
      customArgs: {
        channel,
        totalLatency,
        stageDur: dur,
      },
    };
  }

  private createStageArgs(
    event: RelatedEvent | undefined,
    prevEvent?: RelatedEvent,
  ) {
    if (!event) return null;
    return {
      delta: prevEvent ? event.ts - (prevEvent.ts + prevEvent.dur) : null,
      dur: (event.customArgs as {stageDur?: duration})?.stageDur,
      nav: this.createNavTarget(event),
    };
  }

  private createNavTarget(event: RelatedEvent): NavTarget | undefined {
    if (event == undefined) return undefined;
    return {
      id: event.id,
      trackUri: event.trackUri,
      ts: event.ts,
      dur: event.dur,
      depth: event.depth !== undefined ? event.depth : 0,
    };
  }
}
