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

import {QueryResult, QuerySlot, SerialTaskQueue} from '../../base/query_slot';
import {duration, Duration, time, Time} from '../../base/time';
import {getTrackUriForTrackId} from '../../components/related_events/utils';
import {Trace} from '../../public/trace';
import {
  LONG,
  LONG_NULL,
  NUM,
  NUM_NULL,
  STR,
  STR_NULL,
} from '../../trace_processor/query_result';

export interface ContentionState {
  state: string;
  dur: duration;
  count: number;
}

export interface ContentionBlockedFunction {
  func: string;
  dur: duration;
  count: number;
}

export interface LockContentionDetails {
  id: number;
  ts: time;
  dur: duration | null;
  waiterCount: number;
  trackUri?: string;
  ownerTrackUri?: string;
  isMonitor: boolean;

  parentId: number | null;
  binderReplyId: number | null;
  blockingBinderTxnId: number | null;

  lockName: string;

  blockedThreadName: string;
  blockedThreadTid: number | null;
  isBlockedThreadMain: boolean;

  blockingThreadName: string;
  blockingThreadTid: number | null;
  isBlockingThreadMain: boolean;

  blockingTrackUri: string | undefined;

  blockedMethod?: string;
  blockingMethod?: string;

  threadStates: ContentionState[];
  blockedFunctions: ContentionBlockedFunction[];
}

export class AndroidLockContentionEventSource {
  private readonly queue = new SerialTaskQueue();
  private readonly dataSlot = new QuerySlot<LockContentionDetails | null>(
    this.queue,
  );
  private readonly mergedDataSlot = new QuerySlot<LockContentionDetails[]>(
    this.queue,
  );

  constructor(private readonly trace: Trace) {}

  useMerged(mergedId: number): QueryResult<LockContentionDetails[]> {
    return this.mergedDataSlot.use({
      key: {mergedId},
      queryFn: async () => this.fetchMergedDetails(mergedId),
    });
  }

  async fetchMergedDetails(mergedId: number): Promise<LockContentionDetails[]> {
    const tableName = '__android_lock_contention_owner_events';
    const query = await this.trace.engine.query(`
      SELECT owner_tid, ts, dur FROM ${tableName} WHERE id = ${mergedId} LIMIT 1
    `);
    if (query.numRows() === 0) return [];
    const row = query.firstRow({owner_tid: NUM, ts: LONG, dur: LONG});

    const threadTrackQuery = await this.trace.engine.query(`
      SELECT id FROM thread_track WHERE utid = (SELECT utid FROM thread WHERE tid = ${row.owner_tid} LIMIT 1) LIMIT 1
    `);
    let ownerTrackUri: string | undefined = undefined;
    if (threadTrackQuery.numRows() > 0) {
      const trackId = threadTrackQuery.firstRow({id: NUM}).id;
      ownerTrackUri = getTrackUriForTrackId(this.trace, trackId);
    }

    const baseQuery = await this.trace.engine.query(`
      SELECT 
        a.id,
        a.ts,
        a.dur,
        a.lock_name,
        a.owner_tid,
        a.blocked_thread_name,
        a.blocking_thread_name,
        t.tid as blocked_thread_tid,
        t.is_main_thread as is_blocked_thread_main,
        s.track_id,
        m.short_blocked_method,
        m.short_blocking_method,
        m.id IS NOT NULL AS is_monitor
      FROM __android_lock_contention_owner_events a
      JOIN slice s ON a.id = s.id
      JOIN thread_track tt ON s.track_id = tt.id
      JOIN thread t USING (utid)
      LEFT JOIN android_monitor_contention m ON a.id = m.id
      WHERE a.owner_tid = ${row.owner_tid}
        AND a.ts < ${row.ts + row.dur}
        AND a.ts + a.dur > ${row.ts}
    `);

    const detailsArray: LockContentionDetails[] = [];

    const baseIt = baseQuery.iter({
      id: NUM,
      ts: LONG,
      dur: LONG_NULL,
      lock_name: STR_NULL,
      owner_tid: NUM_NULL,
      blocked_thread_name: STR_NULL,
      blocking_thread_name: STR_NULL,
      blocked_thread_tid: NUM_NULL,
      is_blocked_thread_main: NUM_NULL,
      track_id: NUM_NULL,
      short_blocked_method: STR_NULL,
      short_blocking_method: STR_NULL,
      is_monitor: NUM,
    });

    for (; baseIt.valid(); baseIt.next()) {
      const trackUri =
        baseIt.track_id !== null
          ? getTrackUriForTrackId(this.trace, baseIt.track_id)
          : undefined;

      detailsArray.push({
        id: baseIt.id,
        ts: Time.fromRaw(baseIt.ts),
        dur: baseIt.dur !== null ? Duration.fromRaw(baseIt.dur) : null,
        waiterCount: 0,
        isMonitor: baseIt.is_monitor === 1,
        parentId: null,
        binderReplyId: null,
        blockingBinderTxnId: null,
        lockName: baseIt.lock_name || 'Unknown Lock',
        blockedThreadName: baseIt.blocked_thread_name || 'Unknown Thread',
        blockedThreadTid: baseIt.blocked_thread_tid,
        isBlockedThreadMain: baseIt.is_blocked_thread_main === 1,
        blockingThreadName: baseIt.blocking_thread_name || 'Unknown Thread',
        blockingThreadTid: baseIt.owner_tid ?? null,
        isBlockingThreadMain: false,
        blockingTrackUri: undefined,
        trackUri,
        blockedMethod: baseIt.short_blocked_method || undefined,
        blockingMethod: baseIt.short_blocking_method || undefined,
        ownerTrackUri,
        threadStates: [],
        blockedFunctions: [],
      });
    }

    return detailsArray;
  }

  async fetchThreadStates(contentionId: number): Promise<ContentionState[]> {
    const tsQuery = await this.trace.engine.query(`
      SELECT thread_state, thread_state_dur, thread_state_count
      FROM android_all_lock_contention_thread_state_by_txn
      WHERE id = ${contentionId}
      ORDER BY thread_state_dur DESC
    `);

    const tsIt = tsQuery.iter({
      thread_state: STR,
      thread_state_dur: LONG,
      thread_state_count: NUM,
    });

    const states: ContentionState[] = [];
    while (tsIt.valid()) {
      states.push({
        state: tsIt.thread_state,
        dur: Duration.fromRaw(tsIt.thread_state_dur),
        count: tsIt.thread_state_count,
      });
      tsIt.next();
    }
    return states;
  }

  async fetchBlockedFunctions(
    contentionId: number,
  ): Promise<ContentionBlockedFunction[]> {
    const bfQuery = await this.trace.engine.query(`
      SELECT blocked_function, blocked_function_dur, blocked_function_count
      FROM android_all_lock_contention_blocked_functions_by_txn
      WHERE id = ${contentionId}
      ORDER BY blocked_function_dur DESC
    `);

    const bfIt = bfQuery.iter({
      blocked_function: STR,
      blocked_function_dur: LONG,
      blocked_function_count: NUM,
    });

    const functions: ContentionBlockedFunction[] = [];
    while (bfIt.valid()) {
      functions.push({
        func: bfIt.blocked_function,
        dur: Duration.fromRaw(bfIt.blocked_function_dur),
        count: bfIt.blocked_function_count,
      });
      bfIt.next();
    }
    return functions;
  }

  use(
    eventId: number,
    trackUri: string,
  ): QueryResult<LockContentionDetails | null> {
    return this.dataSlot.use({
      key: {eventId, trackUri},
      queryFn: async () => this.fetchDetails(eventId, trackUri),
    });
  }

  async fetchDetails(
    eventId: number,
    trackUri: string,
  ): Promise<LockContentionDetails | null> {
    let resolvedId = eventId;
    const debugMatch = trackUri.match(/^debug\.track(\d+)(?:_\d+)?$/);

    const ownerTrackPrefix = 'com.android.AndroidLockContention#OwnerEvents';
    if (trackUri.startsWith(ownerTrackPrefix)) {
      resolvedId = eventId;
    } else if (debugMatch) {
      const tableId = debugMatch[1];
      const tableName = `__debug_track_${tableId}`;
      const query = await this.trace.engine.query(`
        SELECT raw_original_id FROM ${tableName} WHERE id = ${eventId} LIMIT 1
      `);
      if (query.numRows() > 0) {
        resolvedId = query.firstRow({raw_original_id: NUM}).raw_original_id;
      }
    }

    const monitorQuery = await this.trace.engine.query(`
      SELECT 
        id, ts, dur, lock_name, waiter_count,
        blocked_thread_name, blocking_thread_name, blocking_thread_tid as owner_tid,
        short_blocked_method, short_blocking_method,
        parent_id, binder_reply_id, blocking_utid,
        (SELECT id FROM thread_track WHERE utid = blocking_utid) as blocking_track_id
      FROM android_monitor_contention_chain
      WHERE id = ${resolvedId}
      LIMIT 1
    `);

    const isMonitor = monitorQuery.numRows() > 0;
    let row;
    let blockedMethod = '';
    let blockingMethod = '';
    let waiterCount = 0;
    let parentId: number | null = null;
    let binderReplyId: number | null = null;
    let blockingBinderTxnId: number | null = null;

    if (isMonitor) {
      const monitorRow = monitorQuery.firstRow({
        id: NUM,
        ts: LONG,
        dur: LONG_NULL,
        lock_name: STR,
        waiter_count: NUM,
        blocked_thread_name: STR_NULL,
        blocking_thread_name: STR_NULL,
        owner_tid: NUM_NULL,
        short_blocked_method: STR_NULL,
        short_blocking_method: STR_NULL,
        parent_id: NUM_NULL,
        binder_reply_id: NUM_NULL,
        blocking_utid: NUM_NULL,
        blocking_track_id: NUM_NULL,
      });
      row = monitorRow;
      blockedMethod = monitorRow.short_blocked_method || '';
      blockingMethod = monitorRow.short_blocking_method || '';
      waiterCount = monitorRow.waiter_count;
      parentId = monitorRow.parent_id;
      binderReplyId = monitorRow.binder_reply_id;

      if (monitorRow.blocking_track_id !== null) {
        const binderQuery = await this.trace.engine.query(`
          SELECT id 
          FROM slice 
          WHERE track_id = ${monitorRow.blocking_track_id} 
            AND name = 'binder transaction' 
            AND ts < ${monitorRow.ts + (monitorRow.dur ?? 0n)} 
            AND (ts + dur) > ${monitorRow.ts}
          LIMIT 1
        `);
        if (binderQuery.numRows() > 0) {
          blockingBinderTxnId = binderQuery.firstRow({id: NUM}).id;
        }
      }
    } else {
      const query = await this.trace.engine.query(`
        SELECT 
          id, ts, dur, name AS lock_name, owner_tid,
          blocked_thread_name, blocking_thread_name
        FROM android_all_lock_contentions
        WHERE id = ${resolvedId}
        LIMIT 1
      `);
      if (query.numRows() === 0) return null;
      row = query.firstRow({
        id: NUM,
        ts: LONG,
        dur: LONG_NULL,
        lock_name: STR,
        owner_tid: NUM_NULL,
        blocked_thread_name: STR_NULL,
        blocking_thread_name: STR_NULL,
      });
    }

    let blockingTrackUri: string | undefined = undefined;
    if (row.owner_tid !== null) {
      blockingTrackUri = `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`;
    }

    return {
      id: row.id,
      ts: Time.fromRaw(row.ts),
      dur: row.dur !== null ? Duration.fromRaw(row.dur) : null,
      waiterCount,
      isMonitor,
      lockName: row.lock_name,

      parentId,
      binderReplyId,
      blockingBinderTxnId,

      blockedThreadName: row.blocked_thread_name || 'Unknown Thread',
      blockedThreadTid: null,
      isBlockedThreadMain: false,

      blockingThreadName: row.blocking_thread_name || 'Unknown Thread',
      blockingThreadTid: row.owner_tid,
      isBlockingThreadMain: false,

      blockingTrackUri,
      trackUri,

      blockedMethod,
      blockingMethod,

      threadStates: [],
      blockedFunctions: [],
    };
  }
}
