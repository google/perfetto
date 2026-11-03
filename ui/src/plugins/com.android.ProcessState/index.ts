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

import {HSLColor} from '../../base/color';
import {getOrCreate} from '../../base/utils';
import {createAggregationTab} from '../../components/aggregation_adapter';
import {
  getColorForSlice,
  GRAY,
  makeColorScheme,
} from '../../components/colorizer';
import {CounterTrack} from '../../components/tracks/counter_track';
import {SliceTrack} from '../../components/tracks/slice_track';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import type {TrackRenderer} from '../../public/track';
import {TrackNode} from '../../public/workspace';
import {SourceDataset} from '../../trace_processor/dataset';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import ProcessThreadGroupsPlugin from '../dev.perfetto.ProcessThreadGroups';
import {
  bindsToPeerTrackUri,
  bindsToTrackUri,
  boundByPeerTrackUri,
  boundByTrackUri,
  ProcessExitsAggregator,
  ProcessStartsAggregator,
  ProcessStateResidencyAggregator,
  ProcessStateTransitionsAggregator,
  processTrackUri,
  providerBindsToPeerTrackUri,
  providerBindsToTrackUri,
  providerBoundByPeerTrackUri,
  providerBoundByTrackUri,
  TAG_BINDS_TO_TRACK,
  TAG_BOUND_BY_TRACK,
  TAG_COUNT_TRACK,
  TAG_PROCESS_TRACK,
} from './aggregators';
import {
  BindingSliceDetailsPanel,
  ProcessStateDetailsPanel,
} from './details_panel';
import {ProcessStateCountTrack} from './state_count_track';

const BASE_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  upid: NUM,
  pid: NUM,
  uid: NUM_NULL,
  user_id: NUM_NULL,
  process_name: STR_NULL,
  package_name: STR_NULL,
  version_code: NUM_NULL,
  debuggable: NUM_NULL,
  state: STR,
  state_change_reason: STR_NULL,
} as const;

const NONEXISTENT_SCHEMA = {
  ...BASE_SCHEMA,
  hosting_type: STR_NULL,
  hosting_name: STR_NULL,
  trigger_type: STR_NULL,
  bind_application_delay_ms: LONG_NULL,
  process_start_delay_ms: LONG_NULL,
} as const;

const EXITED_SCHEMA = {
  ...BASE_SCHEMA,
  exit_reason: STR_NULL,
  exit_subreason: STR_NULL,
} as const;

const PROCESS_STATE_SCHEMA = {...NONEXISTENT_SCHEMA, ...EXITED_SCHEMA} as const;

const FREEZER_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  upid: NUM,
  pid: NUM,
  process_name: STR_NULL,
  state: STR,
} as const;

const BINDING_SCHEMA = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  binding_kind: STR,
  client_upid: NUM,
  client_pid: NUM,
  client_uid: NUM_NULL,
  client_process_name: STR_NULL,
  server_upid: NUM,
  server_pid: NUM,
  server_uid: NUM_NULL,
  server_process_name: STR_NULL,
  target: STR,
  intent_action: STR_NULL,
  bind_flags: STR_NULL,
  stability: STR_NULL,
} as const;

interface BindingPeer {
  readonly peerUpid: number;
  readonly peerPid: number;
  readonly peerName: string | null;
}

const SLATE = makeColorScheme(new HSLColor([210, 18, 48]));

// The per-process tracks come in their hundreds, so keep them compact.
const COMPACT_SLICES = {sliceHeight: 12, titleSizePx: 10};

// Below every other process-scoped track (-50..30) so it appears first.
const PROCESS_GROUP_SORT_ORDER = -100;

// Keeps the freezer and binding lanes out of the aggregators, which scope by
// the state track's tag.
const TAG_FREEZER_TRACK = 'android_freezer';

/**
 * Visualizes Android framework process states over the lifetime of a trace.
 *
 * A process's state timeline lives in its process group. It is also mirrored
 * under one dedicated top level group, which in long memory traces is the only
 * place most of them can appear: those processes carry no other track data,
 * and so get no process group at all.
 */
export default class ProcessState implements PerfettoPlugin {
  static readonly id = 'com.android.ProcessState';
  static readonly description =
    'Visualizes Android framework process states: per-state concurrency ' +
    'counters, and per-process state, freezer, and binding timelines.';
  static readonly dependencies = [ProcessThreadGroupsPlugin];

  async onTraceLoad(ctx: Trace): Promise<void> {
    if (!(await this.hasProcessStateData(ctx))) {
      return;
    }

    const hasBindings = await this.hasProcessBindingData(ctx);
    await ctx.engine.query(`
      INCLUDE PERFETTO MODULE android.process_state;
      INCLUDE PERFETTO MODULE android.freezer;
      ${hasBindings ? 'INCLUDE PERFETTO MODULE android.process_bindings;' : ''}
    `);

    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessStateResidencyAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessStateTransitionsAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessStartsAggregator(ctx)),
    );
    ctx.selection.registerAreaSelectionTab(
      createAggregationTab(ctx, new ProcessExitsAggregator(ctx)),
    );

    const group = new TrackNode({
      name: 'Process states',
      isSummary: true,
    });
    group.addChildLast(await this.createConcurrencyTracks(ctx));
    group.addChildLast(await this.createProcessTracks(ctx, hasBindings));
    ctx.defaultWorkspace.addChildInOrder(group);
  }

  // The intrinsic tables only exist if the trace processor build has the
  // android_process_state plugin, and are empty unless the trace actually
  // carries the framework's process state events.
  private async hasProcessStateData(ctx: Trace): Promise<boolean> {
    const result = await ctx.engine.tryQuery(
      `SELECT count() AS cnt FROM __intrinsic_android_process_state`,
    );
    return result.ok && result.value.firstRow({cnt: NUM}).cnt > 0;
  }

  private async hasProcessBindingData(ctx: Trace): Promise<boolean> {
    const result = await ctx.engine.query(`
      SELECT
        EXISTS(SELECT 1 FROM slice WHERE category = 'service_state')
        OR EXISTS(SELECT 1 FROM slice WHERE category = 'provider_state')
        AS hasBindings
    `);
    return result.firstRow({hasBindings: NUM}).hasBindings > 0;
  }

  // One counter per proc state, showing how many processes were in that state
  // at any point in time. Ordered by declaration order in the proto.
  private async createConcurrencyTracks(ctx: Trace): Promise<TrackNode> {
    const summary = new TrackNode({
      name: 'By state',
      isSummary: true,
    });

    // States entered and left within the same timestamp never reach a
    // concurrency of 1, so they would only ever render a flat zero track.
    const states = await ctx.engine.query(`
      SELECT
        state,
        state_rank AS rank
      FROM _android_process_state_concurrency
      GROUP BY state
      HAVING max(concurrency) > 0
      ORDER BY rank, state
    `);

    for (const it = states.iter({state: STR}); it.valid(); it.next()) {
      const {state} = it;
      const uri = `${ProcessState.id}#count.${state}`;
      ctx.tracks.registerTrack({
        uri,
        renderer: new ProcessStateCountTrack(ctx, uri, state),
        tags: {type: TAG_COUNT_TRACK, state},
        description: `Number of processes concurrently in ${state}.`,
      });
      summary.addChildLast(new TrackNode({uri, name: state}));
    }

    return summary;
  }

  // One timeline per process, with its freezer and binding timelines as
  // children when present. Ordered by uid so that the successive
  // processes of a respawning app sit next to each other - that adjacency is
  // the only thing tying an app's lifetimes together, since each restart gets
  // its own upid.
  private async createProcessTracks(
    ctx: Trace,
    hasBindings: boolean,
  ): Promise<TrackNode> {
    const byProcess = new TrackNode({name: 'By process', isSummary: true});
    const groups = ctx.plugins.getPlugin(ProcessThreadGroupsPlugin);

    const processes = await ctx.engine.query(`
      SELECT
        upid,
        pid,
        process_name AS name,
        upid IN (SELECT upid FROM _android_freezer_intervals)
          AS hasFreezer
      FROM _android_process_state_intervals
      GROUP BY upid
      ORDER BY
        uid NULLS LAST,
        min(ts),
        pid
    `);

    const serviceBoundByPeersByUpid = new Map<number, BindingPeer[]>();
    const serviceBindsToPeersByUpid = new Map<number, BindingPeer[]>();
    const providerBoundByPeersByUpid = new Map<number, BindingPeer[]>();
    const providerBindsToPeersByUpid = new Map<number, BindingPeer[]>();
    if (hasBindings) {
      const collectPeers = async (
        sql: string,
        serviceOut: Map<number, BindingPeer[]>,
        providerOut: Map<number, BindingPeer[]>,
      ) => {
        const res = await ctx.engine.query(sql);
        for (
          const it = res.iter({
            binding_kind: STR,
            upid: NUM,
            peer_upid: NUM,
            peer_pid: NUM,
            peer_name: STR_NULL,
          });
          it.valid();
          it.next()
        ) {
          const out = it.binding_kind === 'provider' ? providerOut : serviceOut;
          getOrCreate(out, it.upid, () => []).push({
            peerUpid: it.peer_upid,
            peerPid: it.peer_pid,
            peerName: it.peer_name,
          });
        }
      };

      await collectPeers(
        `
          SELECT
            binding_kind,
            server_upid AS upid,
            client_upid AS peer_upid,
            client_pid AS peer_pid,
            client_process_name AS peer_name
          FROM _android_process_bindings
          GROUP BY binding_kind, server_upid, client_upid
          ORDER BY
            binding_kind,
            server_upid,
            client_uid NULLS LAST,
            client_process_name NULLS LAST,
            min(ts),
            client_pid
        `,
        serviceBoundByPeersByUpid,
        providerBoundByPeersByUpid,
      );
      await collectPeers(
        `
          SELECT
            binding_kind,
            client_upid AS upid,
            server_upid AS peer_upid,
            server_pid AS peer_pid,
            server_process_name AS peer_name
          FROM _android_process_bindings
          GROUP BY binding_kind, client_upid, server_upid
          ORDER BY
            binding_kind,
            client_upid,
            server_uid NULLS LAST,
            server_process_name NULLS LAST,
            min(ts),
            server_pid
        `,
        serviceBindsToPeersByUpid,
        providerBindsToPeersByUpid,
      );
    }

    for (
      const it = processes.iter({
        upid: NUM,
        pid: NUM,
        name: STR_NULL,
        hasFreezer: NUM,
      });
      it.valid();
      it.next()
    ) {
      const {upid, pid, hasFreezer} = it;
      const name = it.name ?? `${pid}`;
      const uri = processTrackUri(upid);
      const processGroup = groups.getGroupForProcess(upid);
      const overviewUri = this.registerProcessTrack(ctx, processGroup, {
        uri,
        tag: TAG_PROCESS_TRACK,
        upid,
        description: `Framework process state of ${name} (${pid}) over time.`,
        create: (trackUri, ownerUri) =>
          SliceTrack.create({
            trace: ctx,
            uri: trackUri,
            dataset: new SourceDataset({
              schema: PROCESS_STATE_SCHEMA,
              src: '_android_process_state_intervals',
              filter: {col: 'upid', eq: upid},
            }),
            sliceLayout: COMPACT_SLICES,
            sliceName: (row) => row.state,
            colorizer: (row) => {
              if (row.state === 'NONEXISTENT') return SLATE;
              if (row.state === 'EXITED') return GRAY;
              return getColorForSlice(row.state);
            },
            detailsPanel: (row) => {
              const schema =
                row.state === 'NONEXISTENT'
                  ? NONEXISTENT_SCHEMA
                  : row.state === 'EXITED'
                    ? EXITED_SCHEMA
                    : BASE_SCHEMA;
              return new ProcessStateDetailsPanel(ctx, schema, row, ownerUri);
            },
            rootTableName:
              ownerUri === undefined
                ? '_android_process_state_intervals'
                : undefined,
          }),
      });
      const overviewNode = new TrackNode({
        uri: overviewUri,
        name: `${name} ${pid}`,
      });
      const groupNode = new TrackNode({
        uri,
        name: 'Process state',
        sortOrder: PROCESS_GROUP_SORT_ORDER,
      });

      if (hasFreezer) {
        const freezerUri = `${uri}.freezer`;
        const overviewFreezerUri = this.registerProcessTrack(
          ctx,
          processGroup,
          {
            uri: freezerUri,
            tag: TAG_FREEZER_TRACK,
            upid,
            description: `Freezer state of ${name} (${pid}) over time.`,
            create: (trackUri, ownerUri) =>
              SliceTrack.create({
                trace: ctx,
                uri: trackUri,
                dataset: new SourceDataset({
                  schema: FREEZER_SCHEMA,
                  src: '_android_freezer_intervals',
                  filter: {col: 'upid', eq: upid},
                }),
                sliceLayout: COMPACT_SLICES,
                sliceName: (row) => row.state,
                colorizer: (row) => getColorForSlice(row.state),
                detailsPanel: (row) =>
                  new ProcessStateDetailsPanel(
                    ctx,
                    FREEZER_SCHEMA,
                    row,
                    ownerUri,
                  ),
                rootTableName:
                  ownerUri === undefined
                    ? '_android_freezer_intervals'
                    : undefined,
              }),
          },
        );
        overviewNode.addChildLast(
          new TrackNode({uri: overviewFreezerUri, name: 'Freezer'}),
        );
        groupNode.addChildLast(
          new TrackNode({uri: freezerUri, name: 'Freezer'}),
        );
      }

      const serviceBoundByPeers = serviceBoundByPeersByUpid.get(upid);
      if (serviceBoundByPeers !== undefined) {
        this.attachBindingTracks(ctx, processGroup, overviewNode, groupNode, {
          upid,
          bindingKind: 'service',
          direction: 'bound_by',
          rootUri: boundByTrackUri(upid),
          rootName: 'Bound by',
          tag: TAG_BOUND_BY_TRACK,
          rootDescription: `Number of active incoming service bindings to ${name} (${pid}) over time.`,
          selfCol: 'server_upid',
          peerCol: 'client_upid',
          peers: serviceBoundByPeers,
          peerUri: (clientUpid) => boundByPeerTrackUri(upid, clientUpid),
          peerDescription: (peerLabel) =>
            `Incoming service bindings from ${peerLabel} to ${name} (${pid}).`,
        });
      }

      const serviceBindsToPeers = serviceBindsToPeersByUpid.get(upid);
      if (serviceBindsToPeers !== undefined) {
        this.attachBindingTracks(ctx, processGroup, overviewNode, groupNode, {
          upid,
          bindingKind: 'service',
          direction: 'binds_to',
          rootUri: bindsToTrackUri(upid),
          rootName: 'Binds to',
          tag: TAG_BINDS_TO_TRACK,
          rootDescription: `Number of active outgoing service bindings from ${name} (${pid}) over time.`,
          selfCol: 'client_upid',
          peerCol: 'server_upid',
          peers: serviceBindsToPeers,
          peerUri: (serverUpid) => bindsToPeerTrackUri(upid, serverUpid),
          peerDescription: (peerLabel) =>
            `Outgoing service bindings from ${name} (${pid}) to ${peerLabel}.`,
        });
      }

      const providerBoundByPeers = providerBoundByPeersByUpid.get(upid);
      if (providerBoundByPeers !== undefined) {
        this.attachBindingTracks(ctx, processGroup, overviewNode, groupNode, {
          upid,
          bindingKind: 'provider',
          direction: 'bound_by',
          rootUri: providerBoundByTrackUri(upid),
          rootName: 'Provider bound by',
          tag: TAG_BOUND_BY_TRACK,
          rootDescription: `Number of active incoming content provider connections to ${name} (${pid}) over time.`,
          selfCol: 'server_upid',
          peerCol: 'client_upid',
          peers: providerBoundByPeers,
          peerUri: (clientUpid) =>
            providerBoundByPeerTrackUri(upid, clientUpid),
          peerDescription: (peerLabel) =>
            `Incoming content provider connections from ${peerLabel} to ${name} (${pid}).`,
        });
      }

      const providerBindsToPeers = providerBindsToPeersByUpid.get(upid);
      if (providerBindsToPeers !== undefined) {
        this.attachBindingTracks(ctx, processGroup, overviewNode, groupNode, {
          upid,
          bindingKind: 'provider',
          direction: 'binds_to',
          rootUri: providerBindsToTrackUri(upid),
          rootName: 'Provider binds to',
          tag: TAG_BINDS_TO_TRACK,
          rootDescription: `Number of active outgoing content provider connections from ${name} (${pid}) over time.`,
          selfCol: 'client_upid',
          peerCol: 'server_upid',
          peers: providerBindsToPeers,
          peerUri: (serverUpid) =>
            providerBindsToPeerTrackUri(upid, serverUpid),
          peerDescription: (peerLabel) =>
            `Outgoing content provider connections from ${name} (${pid}) to ${peerLabel}.`,
        });
      }

      byProcess.addChildLast(overviewNode);
      processGroup?.addChildInOrder(groupNode);
    }

    return byProcess;
  }

  private attachBindingTracks(
    ctx: Trace,
    processGroup: TrackNode | undefined,
    overviewParent: TrackNode,
    groupParent: TrackNode,
    opts: {
      upid: number;
      bindingKind: 'service' | 'provider';
      direction: 'bound_by' | 'binds_to';
      rootUri: string;
      rootName: string;
      tag: string;
      rootDescription: string;
      selfCol: 'server_upid' | 'client_upid';
      peerCol: 'client_upid' | 'server_upid';
      peers: ReadonlyArray<BindingPeer>;
      peerUri: (peerUpid: number) => string;
      peerDescription: (peerLabel: string) => string;
    },
  ): void {
    const overviewRootUri = this.registerProcessTrack(ctx, processGroup, {
      uri: opts.rootUri,
      tag: opts.tag,
      upid: opts.upid,
      description: opts.rootDescription,
      create: (trackUri) =>
        CounterTrack.create({
          trace: ctx,
          uri: trackUri,
          sqlSource: `
            SELECT ts, count AS value
            FROM _android_process_binding_counts
            WHERE binding_kind = '${opts.bindingKind}'
              AND direction = '${opts.direction}'
              AND upid = ${opts.upid}
          `,
        }),
    });
    const overviewRootNode = new TrackNode({
      uri: overviewRootUri,
      name: opts.rootName,
    });
    const groupRootNode = new TrackNode({
      uri: opts.rootUri,
      name: opts.rootName,
    });

    for (const peer of opts.peers) {
      const peerLabel =
        peer.peerName !== null
          ? `${peer.peerName} ${peer.peerPid}`
          : `${peer.peerPid}`;
      const childUri = opts.peerUri(peer.peerUpid);
      const overviewChildUri = this.registerProcessTrack(ctx, processGroup, {
        uri: childUri,
        tag: opts.tag,
        upid: opts.upid,
        description: opts.peerDescription(peerLabel),
        create: (trackUri, ownerUri) =>
          SliceTrack.create({
            trace: ctx,
            uri: trackUri,
            dataset: new SourceDataset({
              schema: BINDING_SCHEMA,
              src: `
                SELECT *
                FROM _android_process_bindings
                WHERE binding_kind = '${opts.bindingKind}'
                  AND ${opts.selfCol} = ${opts.upid}
              `,
              filter: {col: opts.peerCol, eq: peer.peerUpid},
            }),
            sliceLayout: COMPACT_SLICES,
            sliceName: (row) => row.target,
            colorizer: (row) => getColorForSlice(row.target),
            detailsPanel: (row) =>
              new BindingSliceDetailsPanel(ctx, row, opts.direction, ownerUri),
          }),
      });
      overviewRootNode.addChildLast(
        new TrackNode({uri: overviewChildUri, name: peerLabel}),
      );
      groupRootNode.addChildLast(
        new TrackNode({uri: childUri, name: peerLabel}),
      );
    }

    overviewParent.addChildLast(overviewRootNode);
    groupParent.addChildLast(groupRootNode);
  }

  // Registers a process-scoped track. The process group owns it when there is
  // one; the 'By process' overview then gets a mirror under `${uri}.overview`
  // that links back to the owner, which alone keeps rootTableName so that
  // selecting a slice by id lands on it. Returns the uri for the overview node.
  private registerProcessTrack(
    ctx: Trace,
    processGroup: TrackNode | undefined,
    track: {
      uri: string;
      tag: string;
      upid: number;
      description: string;
      create: (trackUri: string, ownerUri?: string) => TrackRenderer;
    },
  ): string {
    const {uri, tag, upid, description, create} = track;
    const overviewUri = processGroup !== undefined ? `${uri}.overview` : uri;
    for (const trackUri of processGroup ? [uri, overviewUri] : [uri]) {
      const isOwner = trackUri === uri;
      ctx.tracks.registerTrack({
        uri: trackUri,
        renderer: create(trackUri, isOwner ? undefined : uri),
        tags: {type: tag, upid},
        description,
      });
    }
    return overviewUri;
  }
}
