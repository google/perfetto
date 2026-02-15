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
  RelatedEventData,
  RelatedEvent,
  Relation,
  getTrackUriForTrackId,
  NavTarget,
} from '../dev.perfetto.RelatedEvents';
import {time, duration} from '../../base/time';
import {enrichDepths} from '../dev.perfetto.RelatedEvents/utils';
import {STR, NUM_NULL} from '../../trace_processor/query_result';

export type OnDataLoadedCallback = (data: RelatedEventData) => void;

export class AndroidInputEventSource implements EventSource {
  private onDataLoadedCallback?: OnDataLoadedCallback;

  constructor(private trace: Trace) {}

  setOnDataLoadedCallback(callback: OnDataLoadedCallback) {
    this.onDataLoadedCallback = callback;
  }

  async getRelatedEventData(eventId: number): Promise<RelatedEventData> {
    const result = await this.trace.engine.query(
      `SELECT * FROM _android_input_lifecycle_by_slice_id(${eventId})`,
    );

    const events: RelatedEvent[] = [];
    const relations: Relation[] = [];
    const overlayEvents: RelatedEvent[] = [];
    const overlayRelations: Relation[] = [];

    const it = result.iter({
      input_id: STR,
      channel: STR,
      total_latency: NUM_NULL,

      ts_reader: NUM_NULL,
      id_reader: NUM_NULL,
      track_reader: NUM_NULL,
      dur_reader: NUM_NULL,

      ts_dispatch: NUM_NULL,
      id_dispatch: NUM_NULL,
      track_dispatch: NUM_NULL,
      dur_dispatch: NUM_NULL,

      ts_receive: NUM_NULL,
      id_receive: NUM_NULL,
      track_receive: NUM_NULL,
      dur_receive: NUM_NULL,

      ts_consume: NUM_NULL,
      id_consume: NUM_NULL,
      track_consume: NUM_NULL,
      dur_consume: NUM_NULL,

      ts_frame: NUM_NULL,
      id_frame: NUM_NULL,
      track_frame: NUM_NULL,
      dur_frame: NUM_NULL,
    });
    if (!it.valid()) {
      const data = {events: [], relations: [], overlayEvents, overlayRelations};
      this.onDataLoadedCallback?.(data);
      return data;
    }

    const channel = it.channel;
    const totalLatency =
      it.total_latency !== null ? (BigInt(it.total_latency) as duration) : null;

    let readerEvent: RelatedEvent | undefined;
    if (
      it.id_reader !== null &&
      it.ts_reader !== null &&
      it.track_reader !== null &&
      it.dur_reader !== null
    ) {
      const trackUri = getTrackUriForTrackId(this.trace, it.track_reader);
      if (trackUri) {
        readerEvent = {
          id: it.id_reader,
          ts: BigInt(it.ts_reader) as time,
          dur: BigInt(it.dur_reader) as duration,
          trackUri,
          type: 'InputReader',
          customArgs: {
            channel,
            totalLatency,
            stageDur: BigInt(it.dur_reader) as duration,
          },
        };
        overlayEvents.push(readerEvent);
      }
    }

    let dispatchEvent: RelatedEvent | undefined;
    if (
      it.id_dispatch !== null &&
      it.ts_dispatch !== null &&
      it.track_dispatch !== null &&
      it.dur_dispatch !== null
    ) {
      const trackUri = getTrackUriForTrackId(this.trace, it.track_dispatch);
      if (trackUri) {
        dispatchEvent = {
          id: it.id_dispatch,
          ts: BigInt(it.ts_dispatch) as time,
          dur: BigInt(it.dur_dispatch) as duration,
          trackUri,
          type: 'InputDispatcher',
          customArgs: {
            channel,
            totalLatency,
            stageDur: BigInt(it.dur_dispatch) as duration,
          },
        };
        overlayEvents.push(dispatchEvent);
      }
    }

    let receiveEvent: RelatedEvent | undefined;
    if (
      it.id_receive !== null &&
      it.ts_receive !== null &&
      it.track_receive !== null &&
      it.dur_receive !== null
    ) {
      const trackUri = getTrackUriForTrackId(this.trace, it.track_receive);
      if (trackUri) {
        receiveEvent = {
          id: it.id_receive,
          ts: BigInt(it.ts_receive) as time,
          dur: BigInt(it.dur_receive) as duration,
          trackUri,
          type: 'AppReceive',
          customArgs: {
            channel,
            totalLatency,
            stageDur: BigInt(it.dur_receive) as duration,
          },
        };
        overlayEvents.push(receiveEvent);
      }
    }

    let consumeEvent: RelatedEvent | undefined;
    if (
      it.id_consume !== null &&
      it.ts_consume !== null &&
      it.track_consume !== null &&
      it.dur_consume !== null
    ) {
      const trackUri = getTrackUriForTrackId(this.trace, it.track_consume);
      if (trackUri) {
        consumeEvent = {
          id: it.id_consume,
          ts: BigInt(it.ts_consume) as time,
          dur: BigInt(it.dur_consume) as duration,
          trackUri,
          type: 'AppConsume',
          customArgs: {
            channel,
            totalLatency,
            stageDur: BigInt(it.dur_consume) as duration,
          },
        };
        overlayEvents.push(consumeEvent);
      }
    }

    let frameEvent: RelatedEvent | undefined;
    if (
      it.id_frame !== null &&
      it.ts_frame !== null &&
      it.track_frame !== null &&
      it.dur_frame !== null
    ) {
      const trackUri = getTrackUriForTrackId(this.trace, it.track_frame);
      if (trackUri) {
        frameEvent = {
          id: it.id_frame,
          ts: BigInt(it.ts_frame) as time,
          dur: BigInt(it.dur_frame) as duration,
          trackUri,
          type: 'AppFrame',
          customArgs: {
            channel,
            totalLatency,
            stageDur: BigInt(it.dur_frame) as duration,
          },
        };
        overlayEvents.push(frameEvent);
      }
    }

    const stages = [
      readerEvent,
      dispatchEvent,
      receiveEvent,
      consumeEvent,
      frameEvent,
    ];
    for (let i = 0; i < stages.length - 1; i++) {
      const source = stages[i];
      const target = stages[i + 1];
      if (source && target) {
        const relation: Relation = {
          sourceId: source.id,
          targetId: target.id,
          type: 'lifecycle_step',
        };
        overlayRelations.push(relation);
      }
    }

    await enrichDepths(this.trace, overlayEvents);

    const getDelta = (
      start: RelatedEvent | undefined,
      end: RelatedEvent | undefined,
    ): duration | null => {
      if (!start || !end) return null;
      return (end.ts - (start.ts + start.dur)) as duration;
    };

    // This is for the tab view, which shows a single row per channel
    const tabEvent: RelatedEvent = {
      id: eventId,
      ts: (readerEvent?.ts ??
        dispatchEvent?.ts ??
        receiveEvent?.ts ??
        0n) as time,
      dur: (totalLatency ?? 0n) as duration,
      trackUri: '',
      type: 'InputLifecycle',
      customArgs: {
        channel,
        totalLatency,
        reader: readerEvent
          ? {
              dur: readerEvent.customArgs?.stageDur,
              nav: this.createNavTarget(readerEvent),
            }
          : null,
        dispatcher: dispatchEvent
          ? {
              delta: getDelta(readerEvent, dispatchEvent),
              dur: dispatchEvent.customArgs?.stageDur,
              nav: this.createNavTarget(dispatchEvent),
            }
          : null,
        receiver: receiveEvent
          ? {
              delta: getDelta(dispatchEvent, receiveEvent),
              dur: receiveEvent.customArgs?.stageDur,
              nav: this.createNavTarget(receiveEvent),
            }
          : null,
        consumer: consumeEvent
          ? {
              delta: getDelta(receiveEvent, consumeEvent),
              dur: consumeEvent.customArgs?.stageDur,
              nav: this.createNavTarget(consumeEvent),
            }
          : null,
        frame: frameEvent
          ? {
              delta: getDelta(consumeEvent, frameEvent),
              dur: frameEvent.customArgs?.stageDur,
              nav: this.createNavTarget(frameEvent),
            }
          : null,
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

    const data = {events, relations, overlayEvents, overlayRelations};
    this.onDataLoadedCallback?.(data);
    return data;
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
