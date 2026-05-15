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
import {QuerySlot, SerialTaskQueue} from '../../base/query_slot';
import {LockOwnerDetailsPanel} from './lock_owner_details_panel';
import {LOCK_CONTENTION_SQL} from './lock_contention_sql';
import type {Selection} from '../../public/selection';
import {Time} from '../../base/time';
import {TrackNode} from '../../public/workspace';
import {
  LONG,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';
import {HSLColor} from '../../base/color';
import type {ArrowConnection} from '../../components/related_events/arrow_visualiser';
import {
  getTrackUriForTrackId,
  enrichDepths,
  TrackPinningManager,
} from '../../components/related_events/utils';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';
import type {PerfettoPlugin} from '../../public/plugin';
import type {Trace} from '../../public/trace';
import {RelatedEventsOverlay} from '../../components/related_events/related_events_overlay';

export default class AndroidLockContentionPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLockContention';
  static readonly description =
    'Visualise lock contention events in the trace. You can navigate between contention events using ] and [';

  private readonly connectionsTaskQueue = new SerialTaskQueue();
  private readonly connectionsSlot = new QuerySlot<ArrowConnection[]>(
    this.connectionsTaskQueue,
  );
  public highlightedTargetIds = new Set<number>();
  public pinningManager!: TrackPinningManager;
  public currentBlockedSlice?: {id: number; trackUri?: string};
  private lastEventId?: number;

  private async contextualJump(trace: Trace) {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') return;

    const currentEventId = selection.eventId;
    const currentTrackUri = selection.trackUri;

    const query = await trace.engine.query(`
      SELECT owner_tid, id FROM __android_lock_contention_owner_events WHERE id = ${selection.eventId} LIMIT 1
    `);
    if (query.numRows() > 0) {
      const row = query.firstRow({owner_tid: NUM, id: NUM});
      const targetUri = `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`;

      if (currentEventId === row.id && currentTrackUri === targetUri) {
        return;
      }

      this.selectAndNavigate(trace, row.id, targetUri);
      return;
    }

    const contentionQuery = await trace.engine.query(`
      SELECT owner_tid, ts FROM __android_lock_contention_owner_events WHERE id = ${selection.eventId} LIMIT 1
    `);
    if (contentionQuery.numRows() > 0) {
      const row = contentionQuery.firstRow({owner_tid: NUM, ts: LONG});

      const ownerQuery = await trace.engine.query(`
        SELECT id FROM __android_lock_contention_owner_events
        WHERE owner_tid = ${row.owner_tid}
          AND ts <= ${row.ts}
          AND ts + dur >= ${row.ts}
        LIMIT 1
      `);
      if (ownerQuery.numRows() > 0) {
        const ownerId = ownerQuery.firstRow({id: NUM}).id;
        const targetUri = `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`;

        if (currentEventId === ownerId && currentTrackUri === targetUri) {
          return;
        }

        this.selectAndNavigate(trace, ownerId, targetUri);
        return;
      }
    }
  }
  public readonly navigation = new LockContentionNavigation();

  public selectAndNavigate(
    trace: Trace,
    eventId: number,
    trackUri?: string,
    isSqlEvent = false,
  ) {
    const selection = trace.selection.selection;
    if (selection !== undefined) {
      this.navigation.push(selection, eventId, trackUri);
    }

    if (isSqlEvent) {
      trace.selection.selectSqlEvent('slice', eventId, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: false,
      });
    } else if (trackUri) {
      trace.selection.selectTrackEvent(trackUri, eventId, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: false,
      });
    }
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    this.pinningManager = new TrackPinningManager(trace);
    await trace.engine.query(LOCK_CONTENTION_SQL);

    trace.tracks.registerOverlay(
      new RelatedEventsOverlay(trace, () => this.getConnections(trace)),
    );

    trace.commands.registerCommand({
      id: 'com.android.AndroidLockContention:ToggleView',
      name: 'Android Lock Contention: Toggle View',
      defaultHotkey: ']',
      callback: () => {
        this.contextualJump(trace);
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.AndroidLockContention:NavigateBackward',
      name: 'Android Lock Contention: Navigate Backward',
      defaultHotkey: '[',
      callback: async () => {
        await this.navigation.goBack(trace, this);
      },
    });

    trace.commands.registerCommand({
      id: 'com.android.visualiseHeldLocks',
      name: 'Lock Contention: Visualise held locks',
      callback: async () => {
        await addDebugSliceTrack({
          trace: trace,
          data: {
            sqlSource: `
                    WITH lock_held_slices AS (
                    SELECT ts, dur, lock_name, utid
                    FROM interval_merge_overlapping_partitioned!((
                        SELECT ts, dur, name AS lock_name, utid
                        FROM thread_slice
                        WHERE dur > 0 AND thread_slice.name GLOB '*_lock_held'
                    ), (lock_name, utid))
                    )
                    SELECT
                    row_number() OVER () AS id,
                    name AS thread_name,
                    lock_name,
                    utid,
                    ts,
                    MIN(LEAD(ts) OVER(PARTITION BY lock_name ORDER BY ts), ts + dur) - ts AS dur
                    FROM lock_held_slices
                    JOIN thread USING (utid)
                `,
          },
          title: 'Held Lock',
          columns: {
            name: 'thread_name',
          },
          pivotOn: 'lock_name',
        });
      },
    });

    const tableName = '__android_lock_contention_owner_events';
    const tidsQuery = await trace.engine.query(`
      WITH unique_owners AS (
        SELECT owner_tid, MAX(depth) AS max_depth 
        FROM __android_lock_contention_owner_events 
        WHERE owner_tid IS NOT NULL
        GROUP BY owner_tid
      )
      SELECT 
        uo.owner_tid, 
        uo.max_depth,
        t.name,
        tt.id as track_id
      FROM unique_owners uo
      LEFT JOIN thread t ON t.tid = uo.owner_tid
      LEFT JOIN thread_track tt ON tt.utid = t.utid
      GROUP BY uo.owner_tid
    `);
    const tidsIt = tidsQuery.iter({
      owner_tid: NUM,
      max_depth: NUM_NULL,
      name: STR_NULL,
      track_id: NUM_NULL,
    });

    for (; tidsIt.valid(); tidsIt.next()) {
      const tid = tidsIt.owner_tid;
      if (tid === null) continue; // Skip invalid TIDs

      this.registerOwnerTrack(
        trace,
        tid,
        tidsIt.name || 'Unknown',
        tidsIt.max_depth ?? 0,
        tidsIt.track_id,
        tableName,
      );
    }
  }

  private getConnections(trace: Trace): ArrowConnection[] {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') {
      this.highlightedTargetIds.clear();
      this.lastEventId = undefined;
      return [];
    }

    if (this.lastEventId !== selection.eventId) {
      this.highlightedTargetIds.clear();
      this.highlightedTargetIds.add(selection.eventId);
      this.lastEventId = selection.eventId;
    }

    const result = this.connectionsSlot.use({
      key: {
        eventId: selection.eventId,
        highlightedTargetIds: Array.from(this.highlightedTargetIds)
          .sort()
          .join(','),
      },
      queryFn: () => this.fetchConnections(trace, selection),
    });

    return result.data ?? [];
  }

  private async fetchConnections(
    trace: Trace,
    selection: Selection & {kind: 'track_event'},
  ): Promise<ArrowConnection[]> {
    const trackUri = selection.trackUri;
    const eventId = selection.eventId;

    if (trackUri.startsWith('com.android.AndroidLockContention#OwnerEvents')) {
      const targetIds = new Set(this.highlightedTargetIds);

      if (targetIds.size === 0) {
        return [];
      }

      const idList = Array.from(targetIds).join(',');
      const rawQuery = await trace.engine.query(`
        SELECT id, ts, dur, depth FROM __android_lock_contention_owner_events
        WHERE id IN (${idList})
      `);

      const targets: Array<{
        id: number;
        trackUri: string;
        depth: number;
        ts: bigint;
        customDepth: number;
      }> = [];
      const rawIt = rawQuery.iter({id: NUM, ts: LONG, dur: LONG, depth: NUM});

      for (; rawIt.valid(); rawIt.next()) {
        const rawId = rawIt.id;
        const rawTs = rawIt.ts;
        const rawDur = rawIt.dur;
        const middleTs = rawTs + rawDur / 2n;
        const customDepth = rawIt.depth;

        const sliceQuery = await trace.engine.query(`
          SELECT track_id FROM slice WHERE id = ${rawId} LIMIT 1
        `);
        if (sliceQuery.numRows() === 0) continue;
        const trackId = sliceQuery.firstRow({track_id: NUM}).track_id;
        const origTrackUri = getTrackUriForTrackId(trace, trackId);
        if (!origTrackUri) continue;

        targets.push({
          id: rawId,
          trackUri: origTrackUri,
          depth: 0,
          ts: middleTs,
          customDepth,
        });
      }

      await enrichDepths(trace, targets);

      const connections: ArrowConnection[] = [];
      for (const target of targets) {
        connections.push({
          start: {
            trackUri: trackUri,
            ts: Time.fromRaw(target.ts),
            depth: target.customDepth,
          },
          end: {
            trackUri: target.trackUri,
            ts: Time.fromRaw(target.ts),
            depth: target.depth,
          },
        });
      }
      return connections;
    }

    const query = await trace.engine.query(`
      SELECT owner_tid, ts, dur, depth FROM __android_lock_contention_owner_events WHERE id = ${eventId} LIMIT 1
    `);
    if (query.numRows() > 0) {
      const row = query.firstRow({
        owner_tid: NUM,
        ts: LONG,
        dur: LONG,
        depth: NUM,
      });
      const middleTs = row.ts + row.dur / 2n;
      const ownerTrackUri = `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`;

      const targets = [{id: eventId, trackUri: trackUri, depth: 0}];
      await enrichDepths(trace, targets);

      return [
        {
          start: {
            trackUri: trackUri,
            ts: Time.fromRaw(middleTs),
            depth: targets[0].depth,
          },
          end: {
            trackUri: ownerTrackUri,
            ts: Time.fromRaw(middleTs),
            depth: row.depth,
          },
        },
      ];
    }

    return [];
  }

  private registerOwnerTrack(
    trace: Trace,
    tid: number,
    threadName: string,
    maxDepth: number,
    trackId: number | null,
    tableName: string,
  ) {
    const ownerTrackUri = `com.android.AndroidLockContention#OwnerEvents_${tid}`;
    const trackName = `${threadName} [${tid}] Blocking Contentions`;

    trace.tracks.registerTrack({
      uri: ownerTrackUri,
      description:
        'Shows slices representing when this thread is blocking other threads',
      renderer: SliceTrack.create({
        trace,
        uri: ownerTrackUri,
        dataset: new SourceDataset({
          schema: {
            id: NUM,
            ts: LONG,
            dur: LONG,
            name: STR,
            depth: NUM,
          },
          src: tableName,
          filter: {
            col: 'owner_tid',
            eq: tid,
          },
        }),
        initialMaxDepth: maxDepth,

        sliceName: (row) => row.name,
        colorizer: (_) => {
          return {
            base: new HSLColor([210, 80, 50]),
            variant: new HSLColor([210, 80, 60]),
            disabled: new HSLColor([210, 80, 50], 0.5),
            textBase: new HSLColor([0, 0, 100]),
            textVariant: new HSLColor([0, 0, 100]),
            textDisabled: new HSLColor([0, 0, 100], 0.5),
          };
        },
        sliceLayout: {
          sliceHeight: 14,
          titleSizePx: 10,
        },
        detailsPanel: (row) => new LockOwnerDetailsPanel(trace, row.id, this),
      }),
    });

    const ownerTrackNode = new TrackNode({
      uri: ownerTrackUri,
      name: trackName,
      removable: true,
    });

    if (trackId !== null) {
      const track = trace.tracks.findTrack((t) =>
        t.tags?.trackIds?.includes(trackId),
      );

      if (track) {
        const threadNode = trace.currentWorkspace.getTrackByUri(track.uri);
        threadNode?.parent?.addChildBefore(ownerTrackNode, threadNode);
      }
    }
  }
}

function selectionsEqual(a: Selection, b: Selection): boolean {
  if (a.kind !== b.kind) return false;
  if (a.kind === 'track_event' && b.kind === 'track_event') {
    return a.eventId === b.eventId && a.trackUri === b.trackUri;
  }
  if (a.kind === 'track' && b.kind === 'track') {
    return a.trackUri === b.trackUri;
  }
  return false;
}

class LockContentionNavigation {
  private stack: {
    source: Selection;
    targetEventId: number;
    targetTrackUri?: string;
  }[] = [];

  push(source: Selection, targetEventId: number, targetTrackUri?: string) {
    const top = this.stack[this.stack.length - 1];
    if (top !== undefined && selectionsEqual(top.source, source)) {
      return;
    }
    this.stack.push({source, targetEventId, targetTrackUri});
  }

  async goBack(trace: Trace, plugin: AndroidLockContentionPlugin) {
    const currentSelection = trace.selection.selection;

    const top = this.stack[this.stack.length - 1];
    if (
      top !== undefined &&
      currentSelection.kind === 'track_event' &&
      currentSelection.eventId === top.targetEventId &&
      (top.targetTrackUri === undefined ||
        currentSelection.trackUri === top.targetTrackUri ||
        currentSelection.trackUri === 'unknown')
    ) {
      this.stack.pop();
      this.restore(trace, top.source);
      return;
    }

    // Fallback: if we are on the owner track, try to go back to the original slice
    if (
      currentSelection.kind === 'track_event' &&
      currentSelection.trackUri.startsWith(
        'com.android.AndroidLockContention#OwnerEvents',
      )
    ) {
      const blockedSlice = plugin.currentBlockedSlice;
      if (blockedSlice && blockedSlice.trackUri) {
        trace.selection.selectTrackEvent(
          blockedSlice.trackUri,
          blockedSlice.id,
          {
            scrollToSelection: true,
            switchToCurrentSelectionTab: false,
          },
        );
        return;
      }
    }

    if (this.stack.length > 0) {
      this.stack = [];
    }
  }

  private restore(trace: Trace, selection: Selection) {
    if (selection.kind === 'track_event') {
      if (selection.trackUri === 'unknown' || selection.trackUri === '') {
        trace.selection.selectSqlEvent('slice', selection.eventId, {
          scrollToSelection: true,
          switchToCurrentSelectionTab: false,
        });
      } else {
        trace.selection.selectTrackEvent(
          selection.trackUri,
          selection.eventId,
          {
            scrollToSelection: true,
            switchToCurrentSelectionTab: false,
          },
        );
      }
    } else if (selection.kind === 'track') {
      trace.selection.selectTrack(selection.trackUri, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: false,
      });
    }
  }

  has(selection: Selection) {
    return this.stack.some((s) => selectionsEqual(s.source, selection));
  }
}
