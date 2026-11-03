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

import './styles.scss';
import m from 'mithril';
import {Time} from '../../base/time';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Selection} from '../../public/selection';
import type {Trace} from '../../public/trace';
import {NUM, STR} from '../../trace_processor/query_result';
import ProcessState from '../com.android.ProcessState';
import {
  TAG_BINDS_TO_TRACK,
  TAG_BOUND_BY_TRACK,
  TAG_COUNT_TRACK,
} from '../com.android.ProcessState/aggregators';
import {
  type ProcessGraphContext,
  ProcessGraphPanel,
  ProcessGraphSession,
} from './process_graph_panel';

const PLUGIN_ID = 'com.android.ProcessGraph';
const SELECTION_TAB_ID = `${PLUGIN_ID}#SelectionTab`;

const PEER_TRACK_URI_RE =
  /^com\.android\.ProcessState#process\.\d+\.(?:(?:provider_)?bound_by|(?:provider_)?binds_to)\.(\d+)(?:\.overview)?$/;

function extractPeerUpid(trackUri: string): number | undefined {
  const match = PEER_TRACK_URI_RE.exec(trackUri);
  return match !== null ? Number(match[1]) : undefined;
}

export function resolveProcessGraphContext(
  ctx: Trace,
  selection: Selection,
  bindingEdgeByRowId: ReadonlyMap<number, string>,
): ProcessGraphContext | undefined {
  if (selection.kind === 'track_event') {
    const track = ctx.tracks.getTrack(selection.trackUri);
    const upid = track?.tags?.upid;
    if (upid === undefined) return undefined;

    const windowStart = selection.ts;
    const rawDur = selection.dur;
    const windowEnd =
      rawDur === undefined || rawDur === 0n
        ? selection.ts
        : rawDur < 0n
          ? ctx.traceInfo.end
          : Time.fromRaw(selection.ts + rawDur);
    const isBindingTrack =
      track?.tags?.type === TAG_BOUND_BY_TRACK ||
      track?.tags?.type === TAG_BINDS_TO_TRACK;
    const preselectedEdgeKey = isBindingTrack
      ? bindingEdgeByRowId.get(selection.eventId)
      : undefined;

    return {
      selectionKey: `event:${selection.trackUri}:${selection.eventId}:${windowStart}:${windowEnd}`,
      windowStart,
      windowEnd: windowEnd < windowStart ? windowStart : windowEnd,
      targetUpids: [upid],
      targetStates: [],
      preselectedEdgeKey,
    };
  }

  if (selection.kind === 'area') {
    const upids = new Set<number>();
    const peerUpids = new Set<number>();
    const states = new Set<string>();
    let allTracksHavePeer = selection.tracks.length > 0;
    for (const track of selection.tracks) {
      if (typeof track.tags?.upid === 'number') {
        upids.add(track.tags.upid);
      }
      const peerUpid = extractPeerUpid(track.uri);
      if (peerUpid !== undefined) {
        peerUpids.add(peerUpid);
      } else {
        allTracksHavePeer = false;
      }
      if (
        track.tags?.type === TAG_COUNT_TRACK &&
        typeof track.tags?.state === 'string'
      ) {
        states.add(track.tags.state);
      }
    }
    // Area drags over a collapsed group include all child tracks; only include
    // peerUpids when the user explicitly dragged over peer child track(s).
    if (allTracksHavePeer) {
      for (const peerUpid of peerUpids) {
        upids.add(peerUpid);
      }
    }
    if (upids.size === 0 && states.size === 0) return undefined;

    const targetUpids = [...upids].sort((a, b) => a - b);
    const targetStates = [...states].sort();
    return {
      selectionKey: `area:${selection.start}:${selection.end}:u=${targetUpids.join(',')}:s=${targetStates.join(',')}`,
      windowStart: selection.start,
      windowEnd: selection.end,
      targetUpids,
      targetStates,
    };
  }

  if (selection.kind === 'track') {
    const tags = ctx.tracks.getTrack(selection.trackUri)?.tags;
    const upid = tags?.upid;
    if (upid === undefined) return undefined;
    const targetSet = new Set<number>([upid]);
    const peerUpid = extractPeerUpid(selection.trackUri);
    if (peerUpid !== undefined) {
      targetSet.add(peerUpid);
    }
    const targetUpids = [...targetSet].sort((a, b) => a - b);
    return {
      selectionKey: `track:${selection.trackUri}:${targetUpids.join(',')}`,
      windowStart: ctx.traceInfo.start,
      windowEnd: ctx.traceInfo.end,
      targetUpids,
      targetStates: [],
    };
  }

  return undefined;
}

export default class ProcessGraphPlugin implements PerfettoPlugin {
  static readonly id = PLUGIN_ID;
  static readonly description =
    'Visualizes contextual Android service and content provider process dependencies.';
  static readonly dependencies = [ProcessState];

  async onTraceLoad(ctx: Trace): Promise<void> {
    const res = await ctx.engine.query(`
      SELECT
        EXISTS(SELECT 1 FROM slice WHERE category = 'service_state')
        OR EXISTS(SELECT 1 FROM slice WHERE category = 'provider_state')
        AS hasBindings
    `);
    if (res.firstRow({hasBindings: NUM}).hasBindings === 0) {
      return;
    }

    await ctx.engine.query(`INCLUDE PERFETTO MODULE android.process_bindings;`);

    const bindingEdgeByRowId = new Map<number, string>();
    const indexRes = await ctx.engine.query(`
      SELECT id, binding_kind, client_upid, server_upid
      FROM _android_process_bindings
    `);
    for (
      const it = indexRes.iter({
        id: NUM,
        binding_kind: STR,
        client_upid: NUM,
        server_upid: NUM,
      });
      it.valid();
      it.next()
    ) {
      bindingEdgeByRowId.set(
        it.id,
        `${it.binding_kind}:${it.client_upid}>${it.server_upid}`,
      );
    }

    const session = new ProcessGraphSession();
    ctx.trash.defer(() => session.dispose());

    ctx.selection.registerSelectionTab({
      id: SELECTION_TAB_ID,
      name: 'Process Graph',
      render: (selection) => {
        const context = resolveProcessGraphContext(
          ctx,
          selection,
          bindingEdgeByRowId,
        );
        if (context === undefined) {
          return undefined;
        }
        return {
          isLoading: false,
          content: m(ProcessGraphPanel, {trace: ctx, session, context}),
        };
      },
    });
  }
}
