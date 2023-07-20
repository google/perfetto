// Copyright (C) 2023 The Android Open Source Project
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

import m from 'mithril';

import {assertExists} from '../../base/logging';
import {Actions} from '../../common/actions';
import {EngineProxy} from '../../common/engine';
import {LONG, NUM} from '../../common/query_result';
import {duration, Time, time} from '../../common/time';
import {Anchor} from '../../frontend/anchor';
import {globals} from '../../frontend/globals';
import {scrollToTrackAndTs} from '../../frontend/scroll_helper';
import {Icons} from '../../frontend/semantic_icons';
import {SliceSqlId} from '../../frontend/sql_types';

import {
  EventLatencyTrack,
} from './event_latency_track';
import {ScrollJankPluginState} from './index';

export interface EventLatencySlice {
  // Chrome slice id for an EventLatency slice.
  sliceId: SliceSqlId;
  // Timestamp of the beginning of this slice in nanoseconds.
  ts: time;
  // Duration of this slice in nanoseconds.
  dur: duration;
}

export async function getEventLatencySlice(
    engine: EngineProxy, id: number): Promise<EventLatencySlice|undefined> {
  const eventLatencyTrack =
      ScrollJankPluginState.getInstance().getTrack(EventLatencyTrack.kind);
  if (eventLatencyTrack == undefined) {
    throw new Error(`${EventLatencyTrack.kind} track is not registered.`);
  }

  const query = await engine.query(`
    SELECT
      id as sliceId,
      ts,
      dur as dur
    FROM ${eventLatencyTrack.sqlTableName}
    WHERE id=${id}`);
  const it = query.iter({
    sliceId: NUM,
    ts: LONG,
    dur: LONG,
  });

  const result: EventLatencySlice[] = [];

  for (; it.valid(); it.next()) {
    result.push({
      sliceId: it.sliceId as SliceSqlId,
      ts: Time.fromRaw(it.ts),
      dur: it.dur,
    });
  }

  if (result.length > 1) {
    throw new Error(`${
        eventLatencyTrack.sqlTableName} table has more than one row with id ${
        id}`);
  }
  if (result.length === 0) {
    return undefined;
  }
  return result[0];
}

export async function getEventLatencyDescendantSlice(
    engine: EngineProxy, id: number, descendant: string|undefined):
    Promise<EventLatencySlice|undefined> {
  const query = await engine.query(`
    SELECT
      id as sliceId,
      ts,
      dur as dur
    FROM descendant_slice(${id})
    WHERE name='${descendant}'`);
  const it = query.iter({
    sliceId: NUM,
    ts: LONG,
    dur: LONG,
  });

  const result: EventLatencySlice[] = [];

  for (; it.valid(); it.next()) {
    result.push({
      sliceId: it.sliceId as SliceSqlId,
      ts: Time.fromRaw(it.ts),
      dur: it.dur,
    });
  }

  const eventLatencyTrack =
      ScrollJankPluginState.getInstance().getTrack(EventLatencyTrack.kind);
  if (eventLatencyTrack == undefined) {
    throw new Error(`${EventLatencyTrack.kind} track is not registered.`);
  }

  if (result.length > 1) {
    throw new Error(`
        Slice table and track view ${
        eventLatencyTrack
            .sqlTableName} has more than one descendant of slice id ${
        id} with name ${descendant}`);
  }
  if (result.length === 0) {
    return undefined;
  }
  return result[0];
}

interface EventLatencySliceRefAttrs {
  id: SliceSqlId;
  ts: time;
  // If not present, a placeholder name will be used.
  name: string;
  chromeSliceTrackId?: number;
}

export class EventLatencySliceRef implements
    m.ClassComponent<EventLatencySliceRefAttrs> {
  view(vnode: m.Vnode<EventLatencySliceRefAttrs>) {
    return m(
        Anchor,
        {
          icon: Icons.UpdateSelection,
          onclick: () => {
            const eventLatencyTrack =
                ScrollJankPluginState.getInstance().getTrack(
                    EventLatencyTrack.kind);
            if (eventLatencyTrack == undefined) {
              throw new Error(
                  `${EventLatencyTrack.kind} track is not registered.`);
            }

            const trackIdx = vnode.attrs.chromeSliceTrackId as number;
            assertExists(trackIdx);
            const uiTrackId = globals.state.uiTrackIdByTraceTrackId[trackIdx];
            if (uiTrackId === undefined) return;
            globals.makeSelection(Actions.selectChromeSlice(
                {id: vnode.attrs.id, trackId: uiTrackId, table: 'slice'}));

            let trackId = '';
            for (const track of Object.values(globals.state.tracks)) {
              if (track.kind === EventLatencyTrack.kind) {
                trackId = track.id;
              }
            }

            if (trackId === '') {
              throw new Error(
                  `Track id for ${EventLatencyTrack.kind} track not found.`);
            }

            scrollToTrackAndTs(trackId, vnode.attrs.ts, true);
          },
        },
        vnode.attrs.name,
    );
  }
}

export function eventLatencySlice(
    state: EventLatencySlice, name: string, chromeSliceTrackId?: number):
    m.Child {
  return m(EventLatencySliceRef, {
    id: state.sliceId,
    ts: state.ts,
    name: name,
    chromeSliceTrackId: chromeSliceTrackId,
  });
}
