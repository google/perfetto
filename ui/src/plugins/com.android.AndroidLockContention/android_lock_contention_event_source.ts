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

interface MonitorRowData {
  readonly id: number;
  readonly ts: bigint;
  readonly dur: bigint | null;
  readonly lock_name: string;
  readonly waiter_count: number;
  readonly blocked_thread_name: string | null;
  readonly blocking_thread_name: string | null;
  readonly owner_tid: number | null;
  readonly short_blocked_method: string | null;
  readonly short_blocking_method: string | null;
  readonly parent_id: number | null;
  readonly binder_reply_id: number | null;
  readonly blocking_src: string | null;
  readonly blocked_src: string | null;
  readonly blocking_utid: number | null;
  readonly blocking_track_id: number | null;
  readonly is_blocking_thread_main: number | null;
}

export interface ContentionState {
  readonly state: string;
  readonly dur: duration;
  readonly count: number;
}

export interface ContentionBlockedFunction {
  readonly func: string;
  readonly dur: duration;
  readonly count: number;
}

export interface LockContentionDetails {
  readonly id: number;
  readonly ts: time;
  readonly dur: duration | undefined;
  readonly waiterCount: number;
  readonly trackUri?: string;
  readonly ownerTrackUri?: string;
  readonly isMonitor: boolean;

  readonly parentId: number | undefined;
  readonly binderReplyId: number | undefined;
  readonly blockingBinderTxnId: number | undefined;

  readonly lockName: string;
  readonly lockType?: string;

  readonly blockedThreadName: string;
  readonly blockedThreadTid: number | undefined;
  readonly isBlockedThreadMain: boolean;

  readonly blockingThreadName: string;
  readonly blockingThreadTid: number | undefined;
  readonly isBlockingThreadMain: boolean;

  readonly blockingTrackUri: string | undefined;

  readonly blockedMethod?: string;
  readonly blockingMethod?: string;
  readonly blockedSrc?: string;
  readonly blockingSrc?: string;

  readonly threadStates: ReadonlyArray<ContentionState>;
  readonly blockedFunctions: ReadonlyArray<ContentionBlockedFunction>;
}

export class AndroidLockContentionEventSource {
  private readonly queue = new SerialTaskQueue();
  private readonly dataSlot = new QuerySlot<LockContentionDetails | null>(
    this.queue,
  );
  private readonly mergedDataSlot = new QuerySlot<LockContentionDetails[]>(
    this.queue,
  );
  private readonly allDetailsSlot = new QuerySlot<{
    details: LockContentionDetails[];
    threadStates: Map<number, ReadonlyArray<ContentionState>>;
    blockedFunctions: Map<number, ReadonlyArray<ContentionBlockedFunction>>;
  }>(this.queue);

  constructor(private readonly trace: Trace) {}

  useMerged(mergedId: number): QueryResult<LockContentionDetails[]> {
    return this.mergedDataSlot.use({
      key: {mergedId},
      queryFn: async () => this.fetchMergedDetails(mergedId),
    });
  }

  useAllDetails(mergedId: number): QueryResult<{
    details: LockContentionDetails[];
    threadStates: Map<number, ReadonlyArray<ContentionState>>;
    blockedFunctions: Map<number, ReadonlyArray<ContentionBlockedFunction>>;
  }> {
    return this.allDetailsSlot.use({
      key: {mergedId},
      queryFn: async () => this.fetchAllDetails(mergedId),
    });
  }

  async fetchAllDetails(mergedId: number): Promise<{
    details: LockContentionDetails[];
    threadStates: Map<number, ReadonlyArray<ContentionState>>;
    blockedFunctions: Map<number, ReadonlyArray<ContentionBlockedFunction>>;
  }> {
    const details = await this.fetchMergedDetails(mergedId);
    const threadStates = new Map<number, ReadonlyArray<ContentionState>>();
    const blockedFunctions = new Map<
      number,
      ReadonlyArray<ContentionBlockedFunction>
    >();
    for (const row of details) {
      if (row.isMonitor) {
        threadStates.set(row.id, await this.fetchThreadStates(row.id));
        blockedFunctions.set(row.id, await this.fetchBlockedFunctions(row.id));
      }
    }
    return {details, threadStates, blockedFunctions};
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
    const ownerTrackUri =
      threadTrackQuery.numRows() > 0
        ? getTrackUriForTrackId(
            this.trace,
            threadTrackQuery.firstRow({id: NUM}).id,
          )
        : undefined;

    const baseQuery = await this.trace.engine.query(`
      SELECT 
        a.id,
        a.ts,
        a.dur,
        a.lock_name,
        a.lock_type,
        a.owner_tid,
        a.blocked_thread_name,
        a.blocking_thread_name,
        t.tid as blocked_thread_tid,
        t.is_main_thread as is_blocked_thread_main,
        s.track_id,
        m.short_blocked_method,
        m.short_blocking_method,
        m.id IS NOT NULL AS is_monitor,
        m.blocking_src,
        m.blocked_src
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
      lock_type: STR_NULL,
      owner_tid: NUM_NULL,
      blocked_thread_name: STR_NULL,
      blocking_thread_name: STR_NULL,
      blocked_thread_tid: NUM_NULL,
      is_blocked_thread_main: NUM_NULL,
      track_id: NUM_NULL,
      short_blocked_method: STR_NULL,
      short_blocking_method: STR_NULL,
      is_monitor: NUM,
      blocking_src: STR_NULL,
      blocked_src: STR_NULL,
    });

    for (; baseIt.valid(); baseIt.next()) {
      const trackUri =
        baseIt.track_id !== null
          ? getTrackUriForTrackId(this.trace, baseIt.track_id)
          : undefined;

      detailsArray.push({
        id: baseIt.id,
        ts: Time.fromRaw(baseIt.ts),
        dur: baseIt.dur !== null ? Duration.fromRaw(baseIt.dur) : undefined,
        waiterCount: 0,
        isMonitor: baseIt.is_monitor === 1,
        parentId: undefined,
        binderReplyId: undefined,
        blockingBinderTxnId: undefined,
        lockName: baseIt.lock_name || '',
        lockType: baseIt.lock_type || undefined,
        blockedThreadName: baseIt.blocked_thread_name || 'Unknown Thread',
        blockedThreadTid: baseIt.blocked_thread_tid ?? undefined,
        isBlockedThreadMain: baseIt.is_blocked_thread_main === 1,
        blockingThreadName: baseIt.blocking_thread_name || 'Unknown Thread',
        blockingThreadTid: baseIt.owner_tid ?? undefined,
        isBlockingThreadMain: false,
        blockingTrackUri: undefined,
        trackUri,
        blockedMethod: baseIt.short_blocked_method || undefined,
        blockingMethod: baseIt.short_blocking_method || undefined,
        blockedSrc: baseIt.blocked_src || undefined,
        blockingSrc: baseIt.blocking_src || undefined,
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
    const resolvedId = await this.resolveEventId(eventId, trackUri);

    const monitorQuery = await this.trace.engine.query(`
      SELECT 
        id, ts, dur, lock_name, waiter_count,
        blocked_thread_name, blocking_thread_name, blocking_thread_tid as owner_tid,
        short_blocked_method, short_blocking_method,
        parent_id, binder_reply_id, blocking_utid,
        blocking_src, blocked_src,
        (SELECT id FROM thread_track WHERE utid = blocking_utid) as blocking_track_id,
        (SELECT is_main_thread FROM thread WHERE utid = blocking_utid) as is_blocking_thread_main
      FROM android_monitor_contention_chain
      WHERE id = ${resolvedId}
      LIMIT 1
    `);

    if (monitorQuery.numRows() > 0) {
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
        blocking_src: STR_NULL,
        blocked_src: STR_NULL,
        is_blocking_thread_main: NUM_NULL,
      });
      return this.fetchMonitorDetails(monitorRow, trackUri);
    } else {
      return this.fetchFallbackDetails(resolvedId, trackUri);
    }
  }

  private async resolveEventId(
    eventId: number,
    trackUri: string,
  ): Promise<number> {
    const debugMatch = trackUri.match(/^debug\.track(\d+)(?:_\d+)?$/);
    const ownerTrackPrefix = 'com.android.AndroidLockContention#OwnerEvents';

    if (trackUri.startsWith(ownerTrackPrefix)) {
      return eventId;
    } else if (debugMatch) {
      const tableId = debugMatch[1];
      const tableName = `__debug_track_${tableId}`;
      const query = await this.trace.engine.query(`
        SELECT raw_original_id FROM ${tableName} WHERE id = ${eventId} LIMIT 1
      `);
      if (query.numRows() > 0) {
        return query.firstRow({raw_original_id: NUM}).raw_original_id;
      }
    }
    return eventId;
  }

  private async fetchMonitorDetails(
    monitorRow: MonitorRowData,
    trackUri: string,
  ): Promise<LockContentionDetails | null> {
    const blockedMethod = monitorRow.short_blocked_method ?? '';
    const blockingMethod = monitorRow.short_blocking_method ?? '';
    const waiterCount = monitorRow.waiter_count;
    const parentId = monitorRow.parent_id ?? undefined;
    const binderReplyId = monitorRow.binder_reply_id ?? undefined;

    const blockingBinderTxnId = await (async () => {
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
          return binderQuery.firstRow({id: NUM}).id;
        }
      }
      return undefined;
    })();

    const blockingTrackUri =
      monitorRow.owner_tid !== null
        ? `com.android.AndroidLockContention#OwnerEvents_${monitorRow.owner_tid}`
        : undefined;

    return {
      id: monitorRow.id,
      ts: Time.fromRaw(monitorRow.ts),
      dur:
        monitorRow.dur !== null ? Duration.fromRaw(monitorRow.dur) : undefined,
      waiterCount,
      isMonitor: true,
      lockName: monitorRow.lock_name,

      parentId,
      binderReplyId,
      blockingBinderTxnId,

      blockedThreadName: monitorRow.blocked_thread_name || 'Unknown Thread',
      blockedThreadTid: undefined,
      isBlockedThreadMain: false,

      blockingThreadName: monitorRow.blocking_thread_name || 'Unknown Thread',
      blockingThreadTid: monitorRow.owner_tid ?? undefined,
      isBlockingThreadMain: monitorRow.is_blocking_thread_main === 1,

      blockingTrackUri,
      trackUri,

      blockedMethod,
      blockingMethod,
      blockedSrc: monitorRow.blocked_src ?? '',
      blockingSrc: monitorRow.blocking_src ?? '',

      threadStates: [],
      blockedFunctions: [],
    };
  }

  private async fetchFallbackDetails(
    resolvedId: number,
    trackUri: string,
  ): Promise<LockContentionDetails | null> {
    const query = await this.trace.engine.query(`
      SELECT 
        c.id, c.ts, c.dur, c.name AS lock_name, c.owner_tid,
        c.blocked_thread_name, c.blocking_thread_name,
        t.is_main_thread as is_blocking_thread_main,
        c.lock_type, c.is_monitor
      FROM android_all_lock_contentions c
      LEFT JOIN thread t ON c.owner_utid = t.utid
      WHERE c.id = ${resolvedId}
      LIMIT 1
    `);
    if (query.numRows() === 0) return null;
    const row = query.firstRow({
      id: NUM,
      ts: LONG,
      dur: LONG_NULL,
      lock_name: STR,
      owner_tid: NUM_NULL,
      blocked_thread_name: STR_NULL,
      blocking_thread_name: STR_NULL,
      is_blocking_thread_main: NUM_NULL,
      lock_type: STR_NULL,
      is_monitor: NUM,
    });

    const blockingTrackUri =
      row.owner_tid !== null
        ? `com.android.AndroidLockContention#OwnerEvents_${row.owner_tid}`
        : undefined;

    return {
      id: row.id,
      ts: Time.fromRaw(row.ts),
      dur: row.dur !== null ? Duration.fromRaw(row.dur) : undefined,
      waiterCount: 0,
      isMonitor: false,
      lockName: row.lock_name || '',
      lockType: row.lock_type || undefined,

      parentId: undefined,
      binderReplyId: undefined,
      blockingBinderTxnId: undefined,

      blockedThreadName: row.blocked_thread_name || 'Unknown Thread',
      blockedThreadTid: undefined,
      isBlockedThreadMain: false,

      blockingThreadName: row.blocking_thread_name || 'Unknown Thread',
      blockingThreadTid: row.owner_tid ?? undefined,
      isBlockingThreadMain: row.is_blocking_thread_main === 1,

      blockingTrackUri,
      trackUri,

      blockedMethod: '',
      blockingMethod: '',

      threadStates: [],
      blockedFunctions: [],
    };
  }
}
