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

import {QuerySlot, SerialTaskQueue} from '../../base/query_slot';
import {LockOwnerDetailsPanel} from './lock_owner_details_panel';
import {TrackNode} from '../../public/workspace';
import {Selection} from '../../public/selection';
import {Time} from '../../base/time';
import {LONG, NUM, STR, STR_NULL} from '../../trace_processor/query_result';
import {HSLColor} from '../../base/color';
import {ArrowConnection} from '../../components/related_events/arrow_visualiser';
import {
  getTrackUriForTrackId,
  enrichDepths,
  TrackPinningManager,
} from '../../components/related_events/utils';
import {SliceTrack} from '../../components/tracks/slice_track';
import {SourceDataset} from '../../trace_processor/dataset';
import {addDebugSliceTrack} from '../../components/tracks/debug_tracks';
import {PerfettoPlugin} from '../../public/plugin';
import {Trace} from '../../public/trace';
import {RelatedEventsOverlay} from '../../components/related_events/related_events_overlay';

export default class AndroidLockContentionPlugin implements PerfettoPlugin {
  static readonly id = 'com.android.AndroidLockContention';
  static readonly description =
    'Visualise lock contention events in the trace. Activate by running the command ' +
    "'Android Lock Contention: Toggle View' or using Ctrl+]. You can then navigate " +
    'between contention events using Ctrl+] and Ctrl+[';

  private readonly connectionsTaskQueue = new SerialTaskQueue();
  private readonly connectionsSlot = new QuerySlot<ArrowConnection[]>(
    this.connectionsTaskQueue,
  );
  public highlightedTargetIds = new Set<number>();
  public readonly pinningManager = new TrackPinningManager();

  /**
   * Navigates to the source of a monitor contention.
   * Jumps to the parent slice in the chain if available, otherwise
   * shifts focus to the track of the blocking thread.
   */
  private async contextualJump(trace: Trace) {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') return;

    const tableName = '__android_lock_contention_owner_events';

    // Case 1: On a debug track -> Jump to original
    if (
      selection.trackUri.startsWith(
        'com.android.AndroidLockContention#OwnerEvents',
      )
    ) {
      const mergedQuery = await trace.engine.query(`
        SELECT ts, owner_tid FROM ${tableName} WHERE id = ${selection.eventId} LIMIT 1
      `);
      if (mergedQuery.numRows() > 0) {
        const mergedRow = mergedQuery.firstRow({ts: LONG, owner_tid: NUM});

        const originalQuery = await trace.engine.query(`
          SELECT id FROM android_all_lock_contentions
          WHERE owner_tid = ${mergedRow.owner_tid}
            AND ts <= ${mergedRow.ts}
            AND ts + dur >= ${mergedRow.ts}
          LIMIT 1
        `);

        if (originalQuery.numRows() > 0) {
          const originalId = originalQuery.firstRow({id: NUM}).id;
          trace.selection.selectSqlEvent('slice', originalId, {
            scrollToSelection: true,
            switchToCurrentSelectionTab: false,
          });
          return;
        }
      }
    }

    // Case 2: On the original track -> Jump to debug
    const query = await trace.engine.query(`
      SELECT owner_tid, id FROM ${tableName} WHERE id = ${selection.eventId} LIMIT 1
    `);
    if (query.numRows() > 0) {
      const row = query.firstRow({owner_tid: NUM, id: NUM});
      const targetUri = `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`;
      trace.selection.selectTrackEvent(targetUri, row.id, {
        scrollToSelection: true,
        switchToCurrentSelectionTab: false,
      });
      return;
    }

    // Case 3: On a regular lock contention slice -> Jump to owner debug track
    const contentionQuery = await trace.engine.query(`
      SELECT owner_tid, ts FROM android_all_lock_contentions WHERE id = ${selection.eventId} LIMIT 1
    `);
    if (contentionQuery.numRows() > 0) {
      const row = contentionQuery.firstRow({owner_tid: NUM, ts: LONG});
      // Now find the merged owner event that covers this ts!
      const ownerQuery = await trace.engine.query(`
        SELECT id FROM ${tableName}
        WHERE owner_tid = ${row.owner_tid}
          AND ts <= ${row.ts}
          AND ts + dur >= ${row.ts}
        LIMIT 1
      `);
      if (ownerQuery.numRows() > 0) {
        const ownerId = ownerQuery.firstRow({id: NUM}).id;
        const targetUri = `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`;
        trace.selection.selectTrackEvent(targetUri, ownerId, {
          scrollToSelection: true,
          switchToCurrentSelectionTab: false,
        });
        return;
      }
    }
  }

  async onTraceLoad(trace: Trace): Promise<void> {
    await trace.engine.query(
      'INCLUDE PERFETTO MODULE android.monitor_contention;',
    );

    trace.tracks.registerOverlay(
      new RelatedEventsOverlay(trace, () => this.getConnections(trace)),
    );

    trace.commands.registerCommand({
      id: 'com.android.AndroidLockContention:ToggleView',
      name: 'Android Lock Contention: Toggle View',
      defaultHotkey: 'Ctrl+]',
      callback: () => {
        this.contextualJump(trace);
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
    await trace.engine.query(`
      INCLUDE PERFETTO MODULE android.lock_contention;
      INCLUDE PERFETTO MODULE intervals.overlap;

      CREATE PERFETTO TABLE ${tableName} AS
      SELECT 
        row_number() OVER () AS id,
        owner_tid,
        ts,
        dur,
        '[Lock Owner] Blocking' AS name,
        0 AS depth
      FROM interval_merge_overlapping_partitioned!((
        SELECT ts, dur, owner_tid
        FROM android_all_lock_contentions
        WHERE owner_tid IS NOT NULL AND dur > 0
      ), (owner_tid));
    `);

    const tidsQuery = await trace.engine.query(`
      SELECT DISTINCT owner_tid, (SELECT name FROM thread WHERE tid = tid_query.owner_tid LIMIT 1) as name
      FROM ${tableName} tid_query
      WHERE owner_tid IS NOT NULL
    `);
    const tidsIt = tidsQuery.iter({owner_tid: NUM, name: STR_NULL});

    for (; tidsIt.valid(); tidsIt.next()) {
      const tid = tidsIt.owner_tid;
      if (tid === null) continue; // Skip invalid TIDs

      const ownerTrackUri = `com.android.AndroidLockContention#OwnerEvents_${tid}`;

      const threadName = tidsIt.name || 'Unknown';
      const trackName = `${threadName} [${tid}] Lock Owners`;

      // 1. Always register the track
      trace.tracks.registerTrack({
        uri: ownerTrackUri,
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

          sliceName: (row) => row.name,
          colorizer: (_) => {
            // Default to a visible blue-ish color
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

      // 2. Replicate Sched logic: try to find thread track and add sibling
      const threadTrackIdQuery = await trace.engine.query(`
        SELECT t.id FROM thread_track t JOIN thread USING (utid) WHERE tid = ${tid} LIMIT 1
      `);

      if (threadTrackIdQuery.numRows() > 0) {
        const trackId = threadTrackIdQuery.firstRow({id: NUM}).id;
        const track = trace.tracks.findTrack((t) =>
          t.tags?.trackIds?.includes(trackId),
        );

        if (track) {
          const threadNode = trace.currentWorkspace.getTrackByUri(track.uri);
          // Optional chaining to replicate Sched behavior
          threadNode?.parent?.addChildBefore(ownerTrackNode, threadNode);
        }
      }
    }
  }
  private getConnections(trace: Trace): ArrowConnection[] {
    const selection = trace.selection.selection;
    if (selection.kind !== 'track_event') {
      return [];
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

    // Case 1: Custom Owner track selected (New -> Old)
    if (trackUri.startsWith('com.android.AndroidLockContention#OwnerEvents')) {
      if (this.highlightedTargetIds.size === 0) {
        return [];
      }

      const idList = Array.from(this.highlightedTargetIds).join(',');
      const rawQuery = await trace.engine.query(`
        SELECT id, ts FROM android_all_lock_contentions
        WHERE id IN (${idList})
      `);

      const targets: Array<{
        id: number;
        trackUri: string;
        depth: number;
        ts: bigint;
      }> = [];
      const rawIt = rawQuery.iter({id: NUM, ts: LONG});

      for (; rawIt.valid(); rawIt.next()) {
        const rawId = rawIt.id;
        const rawTs = rawIt.ts;

        const sliceQuery = await trace.engine.query(`
          SELECT track_id FROM slice WHERE id = ${rawId} LIMIT 1
        `);
        if (sliceQuery.numRows() === 0) continue;
        const trackId = sliceQuery.firstRow({track_id: NUM}).track_id;
        const origTrackUri = getTrackUriForTrackId(trace, trackId);
        if (!origTrackUri) continue;

        targets.push({id: rawId, trackUri: origTrackUri, depth: 0, ts: rawTs});
      }

      await enrichDepths(trace, targets);

      const connections: ArrowConnection[] = [];
      for (const target of targets) {
        connections.push({
          start: {
            trackUri: trackUri,
            ts: Time.fromRaw(target.ts),
            depth: 0,
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

    // Case 2: Original track selected (Old -> New)
    const rawQuery = await trace.engine.query(`
      SELECT owner_tid, ts FROM android_all_lock_contentions WHERE id = ${eventId} LIMIT 1
    `);
    if (rawQuery.numRows() > 0) {
      const rawRow = rawQuery.firstRow({owner_tid: NUM, ts: LONG});
      const ownerTrackUri = `com.android.AndroidLockContention#OwnerEvents_${rawRow.owner_tid}`;

      const tableName = '__android_lock_contention_owner_events';
      const query = await trace.engine.query(`
        SELECT id FROM ${tableName}
        WHERE owner_tid = ${rawRow.owner_tid}
          AND ts <= ${rawRow.ts}
          AND ts + dur > ${rawRow.ts}
        LIMIT 1
      `);

      if (query.numRows() > 0) {
        const targets = [{id: eventId, trackUri: trackUri, depth: 0}];
        await enrichDepths(trace, targets);

        return [
          {
            start: {
              trackUri: trackUri,
              ts: Time.fromRaw(rawRow.ts),
              depth: targets[0].depth,
            },
            end: {
              trackUri: ownerTrackUri,
              ts: Time.fromRaw(rawRow.ts),
              depth: 0,
            },
          },
        ];
      }
    }

    return [];
  }
}
