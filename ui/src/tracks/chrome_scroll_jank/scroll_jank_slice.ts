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

import {Icons} from '../../base/semantic_icons';
import {duration, time, Time} from '../../base/time';
import {Actions} from '../../common/actions';
import {EngineProxy} from '../../common/engine';
import {LONG, NUM} from '../../common/query_result';
import {globals} from '../../frontend/globals';
import {scrollToTrackAndTs} from '../../frontend/scroll_helper';
import {SliceSqlId} from '../../frontend/sql_types';
import {
  constraintsToQuerySuffix,
  SQLConstraints,
} from '../../frontend/sql_utils';
import {Anchor} from '../../widgets/anchor';

import {EventLatencyTrack} from './event_latency_track';
import {ScrollJankPluginState, ScrollJankTrackSpec} from './index';
import {ScrollJankV3Track} from './scroll_jank_v3_track';

interface BasicSlice {
  // ID of slice.
  sliceId: number;
  // Timestamp of the beginning of this slice in nanoseconds.
  ts: time;
  // Duration of this slice in nanoseconds.
  dur: duration;
}

async function getSlicesFromTrack(
    engine: EngineProxy,
    track: ScrollJankTrackSpec,
    constraints: SQLConstraints): Promise<BasicSlice[]> {
  const query = await engine.query(`
    SELECT
      id AS sliceId,
      ts,
      dur AS dur
    FROM ${track.sqlTableName}
    ${constraintsToQuerySuffix(constraints)}`);
  const it = query.iter({
    sliceId: NUM,
    ts: LONG,
    dur: LONG,
  });

  const result: BasicSlice[] = [];
  for (; it.valid(); it.next()) {
    result.push({
      sliceId: it.sliceId as number,
      ts: Time.fromRaw(it.ts),
      dur: it.dur,
    });
  }
  return result;
}

export type ScrollJankSlice = BasicSlice;
export async function getScrollJankSlices(
    engine: EngineProxy, id: number): Promise<ScrollJankSlice[]> {
  const track =
      ScrollJankPluginState.getInstance().getTrack(ScrollJankV3Track.kind);
  if (track == undefined) {
    throw new Error(`${ScrollJankV3Track.kind} track is not registered.`);
  }

  const slices = await getSlicesFromTrack(engine, track, {
    filters: [`event_latency_id=${id}`],
  });
  return slices;
}

export type EventLatencySlice = BasicSlice;
export async function getEventLatencySlice(
    engine: EngineProxy, id: number): Promise<EventLatencySlice|undefined> {
  const track =
      ScrollJankPluginState.getInstance().getTrack(EventLatencyTrack.kind);
  if (track == undefined) {
    throw new Error(`${EventLatencyTrack.kind} track is not registered.`);
  }

  const slices = await getSlicesFromTrack(engine, track, {
    filters: [`id=${id}`],
  });
  return slices[0];
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

interface BasicScrollJankSliceRefAttrs {
  id: number;
  ts: time;
  dur: duration;
  name: string;
  kind: string;
}

export class ScrollJankSliceRef implements
    m.ClassComponent<BasicScrollJankSliceRefAttrs> {
  view(vnode: m.Vnode<BasicScrollJankSliceRefAttrs>) {
    return m(
        Anchor,
        {
          icon: Icons.UpdateSelection,
          onclick: () => {
            const track =
                ScrollJankPluginState.getInstance().getTrack(vnode.attrs.kind);
            if (track == undefined) {
              throw new Error(`${vnode.attrs.kind} track is not registered.`);
            }

            globals.makeSelection(Actions.selectGenericSlice({
              id: vnode.attrs.id,
              sqlTableName: track.sqlTableName,
              start: vnode.attrs.ts,
              duration: vnode.attrs.dur,
              trackKey: track.key,
              detailsPanelConfig: track.detailsPanelConfig,
            }));

            scrollToTrackAndTs(track.key, vnode.attrs.ts, true);
          },
        },
        vnode.attrs.name,
    );
  }
}

export function getSliceForTrack(
    state: BasicSlice, trackKind: string, name: string): m.Child {
  return m(ScrollJankSliceRef, {
    id: state.sliceId,
    ts: state.ts,
    dur: state.dur,
    name: name,
    kind: trackKind,
  });
}
