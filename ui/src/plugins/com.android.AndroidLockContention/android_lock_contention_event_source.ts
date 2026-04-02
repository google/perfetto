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

export interface ContentionWaiter {
  threadName: string;
  tid: number | null;
  eventId: number;
}

export interface LockContentionDetails {
  id: number;
  ts: time;
  dur: duration | null;
  monotonicDur: duration | null;
  waiterCount: number;
  trackUri?: string;
  ownerTrackUri?: string;
  isMonitor: boolean;

  lockName: string;

  blockedThreadName: string;
  blockedThreadTid: number | null;
  isBlockedThreadMain: boolean;
  blockedMethod: string;
  blockedSrc: string;

  blockingThreadName: string;
  blockingThreadTid: number | null;
  isBlockingThreadMain: boolean;
  blockingMethod: string;
  blockingSrc: string;

  parentId: number | null; // ID of the contention blocking THIS one, if nested

  blockingTrackUri: string | undefined;

  binderReplyId: number | null;
  blockingBinderTxnId: number | null;
  waiters: ContentionWaiter[];

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
        a.name as lock_name,
        a.owner_tid,
        a.blocked_thread_name,
        a.blocking_thread_name,
        t.tid as blocked_thread_tid,
        t.is_main_thread as is_blocked_thread_main,
        m.monotonic_dur,
        m.waiter_count,
        m.short_blocked_method,
        m.blocked_src,
        m.short_blocking_method,
        m.blocking_src,
        m.blocking_utid,
        m.parent_id,
        m.binder_reply_id,
        (SELECT id FROM thread_track WHERE utid = m.blocking_utid LIMIT 1) as blocking_track_id,
        s.track_id,
        m.id IS NOT NULL as is_monitor
      FROM android_all_lock_contentions a
      JOIN slice s ON a.id = s.id
      JOIN thread_track tt ON s.track_id = tt.id
      JOIN thread t USING (utid)
      LEFT JOIN android_monitor_contention_chain m ON a.id = m.id
      WHERE a.owner_tid = ${row.owner_tid}
        AND a.ts < ${row.ts + row.dur}
        AND a.ts + a.dur > ${row.ts}
    `);

    const detailsArray: LockContentionDetails[] = [];
    const monitorIds: number[] = [];

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
      monotonic_dur: LONG_NULL,
      waiter_count: NUM_NULL,
      short_blocked_method: STR_NULL,
      blocked_src: STR_NULL,
      short_blocking_method: STR_NULL,
      blocking_src: STR_NULL,
      blocking_utid: NUM_NULL,
      parent_id: NUM_NULL,
      binder_reply_id: NUM_NULL,
      blocking_track_id: NUM_NULL,
      track_id: NUM_NULL,
      is_monitor: NUM,
    });

    for (; baseIt.valid(); baseIt.next()) {
      const isMonitor = baseIt.is_monitor === 1;
      const id = baseIt.id;
      if (isMonitor) {
        monitorIds.push(id);
      }

      let blockingTrackUri: string | undefined;
      if (baseIt.blocking_track_id !== null) {
        blockingTrackUri = getTrackUriForTrackId(
          this.trace,
          baseIt.blocking_track_id,
        );
      }

      const trackUri =
        baseIt.track_id !== null
          ? getTrackUriForTrackId(this.trace, baseIt.track_id)
          : undefined;

      detailsArray.push({
        id: id,
        ts: Time.fromRaw(baseIt.ts),
        dur: baseIt.dur !== null ? Duration.fromRaw(baseIt.dur) : null,
        monotonicDur:
          baseIt.monotonic_dur !== null
            ? Duration.fromRaw(baseIt.monotonic_dur)
            : null,
        waiterCount: baseIt.waiter_count ?? 0,
        isMonitor: isMonitor,
        lockName: baseIt.lock_name || 'Unknown Lock',
        blockedThreadName: baseIt.blocked_thread_name || 'Unknown Thread',
        blockedThreadTid: baseIt.blocked_thread_tid,
        isBlockedThreadMain: baseIt.is_blocked_thread_main === 1,
        blockedMethod: baseIt.short_blocked_method || '',
        blockedSrc: baseIt.blocked_src || '',
        blockingThreadName: baseIt.blocking_thread_name || 'Unknown Thread',
        blockingThreadTid: baseIt.owner_tid ?? null,
        isBlockingThreadMain: false,
        blockingMethod: baseIt.short_blocking_method || '',
        blockingSrc: baseIt.blocking_src || '',
        parentId: baseIt.parent_id,
        blockingTrackUri,
        trackUri,
        ownerTrackUri,
        binderReplyId: baseIt.binder_reply_id,
        blockingBinderTxnId: null,
        waiters: [],
        threadStates: [],
        blockedFunctions: [],
      });
    }

    if (monitorIds.length === 0) {
      return detailsArray;
    }

    // Fetch waiters for all monitor contentions
    const waitersQuery = await this.trace.engine.query(`
      SELECT 
        b.id as base_id,
        w.blocked_thread_name,
        w.blocked_thread_tid,
        w.id as waiter_event_id
      FROM android_monitor_contention_chain w
      JOIN android_monitor_contention_chain b 
        ON w.blocking_utid = b.blocking_utid
        AND w.short_blocking_method = b.short_blocking_method
        AND w.ts < b.ts + b.dur
        AND w.ts + w.dur > b.ts
        AND w.id != b.id
      WHERE b.id IN (${monitorIds.join(',')})
      ORDER BY w.ts ASC
    `);

    const waitersIt = waitersQuery.iter({
      base_id: NUM,
      blocked_thread_name: STR_NULL,
      blocked_thread_tid: NUM_NULL,
      waiter_event_id: NUM,
    });

    const waitersMap = new Map<number, ContentionWaiter[]>();
    while (waitersIt.valid()) {
      const baseId = waitersIt.base_id;
      const waiter: ContentionWaiter = {
        threadName: waitersIt.blocked_thread_name || 'Unknown Thread',
        tid: waitersIt.blocked_thread_tid,
        eventId: waitersIt.waiter_event_id,
      };
      if (!waitersMap.has(baseId)) {
        waitersMap.set(baseId, []);
      }
      waitersMap.get(baseId)!.push(waiter);
      waitersIt.next();
    }

    // Fetch thread states for all monitor contentions
    const tsQuery = await this.trace.engine.query(`
      SELECT id as contention_id, thread_state, thread_state_dur, thread_state_count
      FROM android_monitor_contention_chain_thread_state_by_txn
      WHERE id IN (${monitorIds.join(',')})
      ORDER BY thread_state_dur DESC
    `);

    const tsIt = tsQuery.iter({
      contention_id: NUM,
      thread_state: STR,
      thread_state_dur: LONG,
      thread_state_count: NUM,
    });

    const statesMap = new Map<number, ContentionState[]>();
    while (tsIt.valid()) {
      const cId = tsIt.contention_id;
      const state: ContentionState = {
        state: tsIt.thread_state,
        dur: Duration.fromRaw(tsIt.thread_state_dur),
        count: tsIt.thread_state_count,
      };
      if (!statesMap.has(cId)) {
        statesMap.set(cId, []);
      }
      statesMap.get(cId)!.push(state);
      tsIt.next();
    }

    // Fetch functions for all monitor contentions
    const bfQuery = await this.trace.engine.query(`
      SELECT id as contention_id, blocked_function, blocked_function_dur, blocked_function_count
      FROM android_monitor_contention_chain_blocked_functions_by_txn
      WHERE id IN (${monitorIds.join(',')})
      ORDER BY blocked_function_dur DESC
    `);

    const bfIt = bfQuery.iter({
      contention_id: NUM,
      blocked_function: STR,
      blocked_function_dur: LONG,
      blocked_function_count: NUM,
    });

    const functionsMap = new Map<number, ContentionBlockedFunction[]>();
    while (bfIt.valid()) {
      const cId = bfIt.contention_id;
      const func: ContentionBlockedFunction = {
        func: bfIt.blocked_function,
        dur: Duration.fromRaw(bfIt.blocked_function_dur),
        count: bfIt.blocked_function_count,
      };
      if (!functionsMap.has(cId)) {
        functionsMap.set(cId, []);
      }
      functionsMap.get(cId)!.push(func);
      bfIt.next();
    }

    // Fetch binder transactions for monitor contentions
    const binderQuery = await this.trace.engine.query(`
      SELECT 
        b.id as base_id,
        s.id as binder_txn_id
      FROM slice s
      JOIN (
        SELECT id, blocking_utid, ts, dur FROM android_monitor_contention_chain WHERE id IN (${monitorIds.join(',')})
      ) b ON s.track_id = (SELECT id FROM thread_track WHERE utid = b.blocking_utid LIMIT 1)
        AND s.name = 'binder transaction'
        AND s.ts < b.ts + b.dur
        AND s.ts + s.dur > b.ts
    `);

    const binderIt = binderQuery.iter({
      base_id: NUM,
      binder_txn_id: NUM,
    });

    const binderMap = new Map<number, number>();
    while (binderIt.valid()) {
      binderMap.set(binderIt.base_id, binderIt.binder_txn_id);
      binderIt.next();
    }

    // Stitch data together!
    for (const details of detailsArray) {
      if (details.isMonitor) {
        details.waiters = waitersMap.get(details.id) || [];
        details.threadStates = statesMap.get(details.id) || [];
        details.blockedFunctions = functionsMap.get(details.id) || [];
        details.blockingBinderTxnId = binderMap.get(details.id) ?? null;
      }
    }
    return detailsArray;
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

    const typeQuery = await this.trace.engine.query(`
      SELECT name, ts, dur, track_id FROM slice WHERE id = ${resolvedId} LIMIT 1
    `);
    if (typeQuery.numRows() === 0) return null;
    const typeRow = typeQuery.firstRow({
      name: STR,
      ts: LONG,
      dur: LONG,
      track_id: NUM,
    });

    const sliceTrackUri =
      typeRow.track_id !== null
        ? getTrackUriForTrackId(this.trace, typeRow.track_id)
        : undefined;

    const monitorQuery = await this.trace.engine.query(`
      SELECT 1 FROM android_monitor_contention_chain WHERE id = ${resolvedId} LIMIT 1
    `);
    const isMonitor = monitorQuery.numRows() > 0;

    if (!isMonitor) {
      const tidMatch = typeRow.name.match(/\(owner tid: (\d+)\)/);
      const ownerTid = tidMatch ? parseInt(tidMatch[1], 10) : undefined;

      const blockedQuery = await this.trace.engine.query(`
        SELECT 
          t.tid as blocked_tid,
          t.name as blocked_name,
          t.is_main_thread,
          p.upid
        FROM slice s
        JOIN thread_track tt ON s.track_id = tt.id
        JOIN thread t USING (utid)
        LEFT JOIN process p USING (upid)
        WHERE s.id = ${resolvedId}
        LIMIT 1
      `);

      let blockedThreadName = 'Unknown Thread';
      let blockedThreadTid: number | null = null;
      let isBlockedThreadMain = false;
      let upid: number | null = null;
      if (blockedQuery.numRows() > 0) {
        const r = blockedQuery.firstRow({
          blocked_tid: NUM,
          blocked_name: STR_NULL,
          is_main_thread: NUM_NULL,
          upid: NUM_NULL,
        });
        blockedThreadTid = r.blocked_tid;
        blockedThreadName = r.blocked_name || 'Unknown Thread';
        isBlockedThreadMain = r.is_main_thread === 1;
        upid = r.upid;
      }

      let blockingThreadName = 'Unknown Thread';
      if (ownerTid !== undefined) {
        // First try same process
        if (upid !== null) {
          const blockingQuery = await this.trace.engine.query(`
            SELECT name FROM thread WHERE tid = ${ownerTid} AND upid = ${upid} LIMIT 1
          `);
          if (blockingQuery.numRows() > 0) {
            blockingThreadName =
              blockingQuery.firstRow({name: STR_NULL}).name || 'Unknown Thread';
          }
        }

        // Fallback to any process if still unknown
        if (blockingThreadName === 'Unknown Thread') {
          const fallbackQuery = await this.trace.engine.query(`
            SELECT name FROM thread WHERE tid = ${ownerTid} LIMIT 1
          `);
          if (fallbackQuery.numRows() > 0) {
            blockingThreadName =
              fallbackQuery.firstRow({name: STR_NULL}).name || 'Unknown Thread';
          }
        }
      }

      let blockingTrackUri: string | undefined = undefined;
      if (ownerTid !== undefined) {
        blockingTrackUri = 'com.android.AndroidLockContention#OwnerEvents';
      }

      return {
        id: resolvedId,
        ts: Time.fromRaw(typeRow.ts),
        dur: typeRow.dur !== null ? Duration.fromRaw(typeRow.dur) : null,
        monotonicDur: null,
        waiterCount: 0,
        isMonitor: false,
        lockName: typeRow.name,
        blockedThreadName,
        blockedThreadTid,
        isBlockedThreadMain,
        blockedMethod: '',
        blockedSrc: '',
        blockingThreadName,
        blockingThreadTid: ownerTid ?? null,
        isBlockingThreadMain: false, // We don't easily know if owner is main thread without another query or join, but it's fine for now.
        blockingMethod: '',
        blockingSrc: '',
        parentId: null,
        blockingTrackUri,
        trackUri: sliceTrackUri,
        binderReplyId: null,
        blockingBinderTxnId: null,
        waiters: [],
        threadStates: [],
        blockedFunctions: [],
      };
    }

    const mainQuery = await this.trace.engine.query(`
      SELECT 
        id, 
        ts, 
        dur, 
        monotonic_dur, 
        waiter_count,
        lock_name,
        
        blocked_thread_name,
        blocked_thread_tid,
        is_blocked_thread_main,
        short_blocked_method,
        blocked_src,
        
        blocking_thread_name,
        blocking_thread_tid,
        is_blocking_thread_main,
        short_blocking_method,
        blocking_src,
        blocking_utid,
        
        parent_id,
        (SELECT id FROM thread_track WHERE utid = blocking_utid) as blocking_track_id,
        binder_reply_id
      FROM android_monitor_contention_chain
      WHERE id = ${resolvedId}
    `);

    if (mainQuery.numRows() === 0) return null;

    const row = mainQuery.firstRow({
      id: NUM,
      ts: LONG,
      dur: LONG_NULL,
      monotonic_dur: LONG_NULL,
      waiter_count: NUM_NULL,
      lock_name: STR_NULL,

      blocked_thread_name: STR_NULL,
      blocked_thread_tid: NUM_NULL,
      is_blocked_thread_main: NUM_NULL,
      short_blocked_method: STR_NULL,
      blocked_src: STR_NULL,

      blocking_thread_name: STR_NULL,
      blocking_thread_tid: NUM_NULL,
      is_blocking_thread_main: NUM_NULL,
      short_blocking_method: STR_NULL,
      blocking_src: STR_NULL,
      blocking_utid: NUM_NULL,

      parent_id: NUM_NULL,
      blocking_track_id: NUM_NULL,
      binder_reply_id: NUM_NULL,
    });

    let blockingTrackUri: string | undefined;
    if (row.blocking_track_id !== null) {
      blockingTrackUri = getTrackUriForTrackId(
        this.trace,
        row.blocking_track_id,
      );
    }

    const blockedFunctions: ContentionBlockedFunction[] = [];
    const bfQuery = await this.trace.engine.query(`
      SELECT blocked_function, blocked_function_dur, blocked_function_count
      FROM android_monitor_contention_chain_blocked_functions_by_txn
      WHERE id = ${eventId}
      ORDER BY blocked_function_dur DESC
    `);
    const bfIt = bfQuery.iter({
      blocked_function: STR,
      blocked_function_dur: LONG,
      blocked_function_count: NUM,
    });
    while (bfIt.valid()) {
      blockedFunctions.push({
        func: bfIt.blocked_function,
        dur: Duration.fromRaw(bfIt.blocked_function_dur),
        count: bfIt.blocked_function_count,
      });
      bfIt.next();
    }

    let blockingBinderTxnId: number | null = null;
    if (row.blocking_track_id !== null) {
      const binderQuery = await this.trace.engine.query(`
        SELECT id 
        FROM slice 
        WHERE track_id = ${row.blocking_track_id} 
          AND name = 'binder transaction' 
          AND ts < ${row.ts + (row.dur ?? 0n)} 
          AND (ts + dur) > ${row.ts}
        LIMIT 1
      `);
      if (binderQuery.numRows() > 0) {
        blockingBinderTxnId = binderQuery.firstRow({id: NUM}).id;
      }
    }

    const waiters: ContentionWaiter[] = [];
    const waitersQuery = await this.trace.engine.query(`
      SELECT 
        blocked_thread_name,
        blocked_thread_tid,
        id 
      FROM android_monitor_contention_chain
      WHERE blocking_utid = ${row.blocking_utid}
        AND short_blocking_method = '${row.short_blocking_method || ''}'
        AND ts < ${row.ts + (row.dur ?? 0n)} 
        AND (ts + dur) > ${row.ts}
        AND id != ${eventId}
      ORDER BY ts ASC
    `);
    const waitersIt = waitersQuery.iter({
      blocked_thread_name: STR_NULL,
      blocked_thread_tid: NUM_NULL,
      id: NUM,
    });
    while (waitersIt.valid()) {
      waiters.push({
        threadName: waitersIt.blocked_thread_name || 'Unknown Thread',
        tid: waitersIt.blocked_thread_tid,
        eventId: waitersIt.id,
      });
      waitersIt.next();
    }

    const threadStates: ContentionState[] = [];
    const tsQuery = await this.trace.engine.query(`
      SELECT thread_state, thread_state_dur, thread_state_count
      FROM android_monitor_contention_chain_thread_state_by_txn
      WHERE id = ${eventId}
      ORDER BY thread_state_dur DESC
    `);
    const tsIt = tsQuery.iter({
      thread_state: STR,
      thread_state_dur: LONG,
      thread_state_count: NUM,
    });
    while (tsIt.valid()) {
      threadStates.push({
        state: tsIt.thread_state,
        dur: Duration.fromRaw(tsIt.thread_state_dur),
        count: tsIt.thread_state_count,
      });
      tsIt.next();
    }

    return {
      id: row.id,
      ts: Time.fromRaw(row.ts),
      dur: row.dur !== null ? Duration.fromRaw(row.dur) : null,
      monotonicDur:
        row.monotonic_dur !== null ? Duration.fromRaw(row.monotonic_dur) : null,
      waiterCount: row.waiter_count ?? 0,
      isMonitor: true,
      lockName: row.lock_name || 'Unknown Lock',

      blockedThreadName: row.blocked_thread_name || 'Unknown Thread',
      blockedThreadTid: row.blocked_thread_tid,
      isBlockedThreadMain: row.is_blocked_thread_main === 1,
      blockedMethod: row.short_blocked_method || 'Unknown Method',
      blockedSrc: row.blocked_src || 'Unknown Source',

      blockingThreadName: row.blocking_thread_name || 'Unknown Thread',
      blockingThreadTid: row.blocking_thread_tid,
      isBlockingThreadMain: row.is_blocking_thread_main === 1,
      blockingMethod: row.short_blocking_method || 'Unknown Method',
      blockingSrc: row.blocking_src || 'Unknown Source',

      parentId: row.parent_id,
      blockingTrackUri,
      trackUri: sliceTrackUri,

      binderReplyId: row.binder_reply_id,
      blockingBinderTxnId,
      waiters,

      threadStates,
      blockedFunctions,
    };
  }
}
