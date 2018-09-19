// Copyright (C) 2018 The Android Open Source Project
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

import '../tracks/all_controller';

import {assertExists, assertTrue} from '../base/logging';
import {
  Action,
  addTrack,
  navigate,
  setEngineReady,
  setTraceTime,
  setVisibleTraceTime,
  updateStatus
} from '../common/actions';
import {TimeSpan} from '../common/time';
import {QuantizedLoad, ThreadDesc} from '../frontend/globals';
import {SLICE_TRACK_KIND} from '../tracks/chrome_slices/common';
import {CPU_SLICE_TRACK_KIND} from '../tracks/cpu_slices/common';

import {Child, Children, Controller} from './controller';
import {Engine} from './engine';
import {globals} from './globals';
import {QueryController, QueryControllerArgs} from './query_controller';
import {TrackControllerArgs, trackControllerRegistry} from './track_controller';

type States = 'init'|'loading_trace'|'ready';


declare interface FileReaderSync { readAsArrayBuffer(blob: Blob): ArrayBuffer; }

declare var FileReaderSync:
    {prototype: FileReaderSync; new (): FileReaderSync;};

// TraceController handles handshakes with the frontend for everything that
// concerns a single trace. It owns the WASM trace processor engine, handles
// tracks data and SQL queries. There is one TraceController instance for each
// trace opened in the UI (for now only one trace is supported).
export class TraceController extends Controller<States> {
  private readonly engineId: string;
  private engine?: Engine;

  constructor(engineId: string) {
    super('init');
    this.engineId = engineId;
  }

  onDestroy() {
    if (this.engine !== undefined) globals.destroyEngine(this.engine.id);
  }

  run() {
    const engineCfg = assertExists(globals.state.engines[this.engineId]);
    switch (this.state) {
      case 'init':
        globals.dispatch(setEngineReady(this.engineId, false));
        this.loadTrace().then(() => {
          globals.dispatch(setEngineReady(this.engineId, true));
        });
        globals.dispatch(updateStatus('Opening trace'));
        this.setState('loading_trace');
        break;

      case 'loading_trace':
        // Stay in this state until loadTrace() returns and marks the engine as
        // ready.
        if (this.engine === undefined || !engineCfg.ready) return;
        this.setState('ready');
        break;

      case 'ready':
        // At this point we are ready to serve queries and handle tracks.
        const engine = assertExists(this.engine);
        assertTrue(engineCfg.ready);
        const childControllers: Children = [];

        // Create a TrackController for each track.
        for (const trackId of Object.keys(globals.state.tracks)) {
          const trackCfg = globals.state.tracks[trackId];
          if (trackCfg.engineId !== this.engineId) continue;
          if (!trackControllerRegistry.has(trackCfg.kind)) continue;
          const trackCtlFactory = trackControllerRegistry.get(trackCfg.kind);
          const trackArgs: TrackControllerArgs = {trackId, engine};
          childControllers.push(Child(trackId, trackCtlFactory, trackArgs));
        }

        // Create a QueryController for each query.
        for (const queryId of Object.keys(globals.state.queries)) {
          const queryArgs: QueryControllerArgs = {queryId, engine};
          childControllers.push(Child(queryId, QueryController, queryArgs));
        }

        return childControllers;

      default:
        throw new Error(`unknown state ${this.state}`);
    }
    return;
  }

  private async loadTrace() {
    globals.dispatch(updateStatus('Creating trace processor'));
    const engineCfg = assertExists(globals.state.engines[this.engineId]);
    this.engine = await globals.createEngine();

    const statusHeader = 'Opening trace';
    if (engineCfg.source instanceof File) {
      const blob = engineCfg.source as Blob;
      const reader = new FileReaderSync();
      const SLICE_SIZE = 1024 * 1024;
      for (let off = 0; off < blob.size; off += SLICE_SIZE) {
        const slice = blob.slice(off, off + SLICE_SIZE);
        const arrBuf = reader.readAsArrayBuffer(slice);
        await this.engine.parse(new Uint8Array(arrBuf));
        const progress = Math.round((off + slice.size) / blob.size * 100);
        globals.dispatch(updateStatus(`${statusHeader} ${progress} %`));
      }
    } else {
      const resp = await fetch(engineCfg.source);
      if (resp.status !== 200) {
        globals.dispatch(updateStatus(`HTTP error ${resp.status}`));
        throw new Error(`fetch() failed with HTTP error ${resp.status}`);
      }
      // tslint:disable-next-line no-any
      const rd = (resp.body as any).getReader() as ReadableStreamReader;
      const tStartMs = performance.now();
      let tLastUpdateMs = 0;
      for (let off = 0;;) {
        const readRes = await rd.read() as {value: Uint8Array, done: boolean};
        if (readRes.value !== undefined) {
          off += readRes.value.length;
          await this.engine.parse(readRes.value);
        }
        // For traces loaded from the network there doesn't seem to be a
        // reliable way to compute the %. The content-length exposed by GCS is
        // before compression (which is handled transparently by the browser).
        const nowMs = performance.now();
        if (nowMs - tLastUpdateMs > 100) {
          tLastUpdateMs = nowMs;
          const mb = off / 1e6;
          const tElapsed = (nowMs - tStartMs) / 1e3;
          let status = `${statusHeader} ${mb.toFixed(1)} MB `;
          status += `(${(mb / tElapsed).toFixed(1)} MB/s)`;
          globals.dispatch(updateStatus(status));
        }
        if (readRes.done) break;
      }
    }

    await this.engine.notifyEof();

    const traceTime = await this.engine.getTraceTimeBounds();
    const actions = [
      setTraceTime(traceTime),
      navigate('/viewer'),
    ];

    if (globals.state.visibleTraceTime.lastUpdate === 0) {
      actions.push(setVisibleTraceTime(traceTime));
    }

    globals.dispatchMultiple(actions);

    await this.listTracks();
    await this.listThreads();
    await this.loadTimelineOverview(traceTime);
  }

  private async listTracks() {
    globals.dispatch(updateStatus('Loading tracks'));
    const engine = assertExists<Engine>(this.engine);
    const addToTrackActions: Action[] = [];
    const numCpus = await engine.getNumberOfCpus();
    for (let cpu = 0; cpu < numCpus; cpu++) {
      addToTrackActions.push(
          addTrack(this.engineId, CPU_SLICE_TRACK_KIND, `Cpu ${cpu}`, {
            cpu,
          }));
    }

    const threadQuery = await engine.rawQuery({
      sqlQuery: 'select upid, utid, tid, thread.name, max(slices.depth) ' +
          'from thread inner join slices using(utid) group by utid'
    });
    for (let i = 0; i < threadQuery.numRecords; i++) {
      const upid = threadQuery.columns[0].longValues![i];
      const utid = threadQuery.columns[1].longValues![i];
      const threadId = threadQuery.columns[2].longValues![i];
      let threadName = threadQuery.columns[3].stringValues![i];
      threadName += `[${threadId}]`;
      const maxDepth = threadQuery.columns[4].longValues![i];
      addToTrackActions.push(
          addTrack(this.engineId, SLICE_TRACK_KIND, threadName, {
            upid: upid as number,
            utid: utid as number,
            maxDepth: maxDepth as number,
          }));
    }
    globals.dispatchMultiple(addToTrackActions);
  }

  private async listThreads() {
    globals.dispatch(updateStatus('Reading thread list'));
    const sqlQuery = 'select utid, tid, pid, thread.name, process.name ' +
        'from thread inner join process using(upid)';
    const threadRows = await assertExists(this.engine).rawQuery({sqlQuery});
    const threads: ThreadDesc[] = [];
    for (let i = 0; i < threadRows.numRecords; i++) {
      const utid = threadRows.columns[0].longValues![i] as number;
      const tid = threadRows.columns[1].longValues![i] as number;
      const pid = threadRows.columns[2].longValues![i] as number;
      const threadName = threadRows.columns[3].stringValues![i];
      const procName = threadRows.columns[4].stringValues![i];
      threads.push({utid, tid, threadName, pid, procName});
    }  // for (record ...)
    globals.publish('Threads', threads);
  }

  private async loadTimelineOverview(traceTime: TimeSpan) {
    const engine = assertExists<Engine>(this.engine);
    const numSteps = 100;
    const stepSec = traceTime.duration / numSteps;
    for (let step = 0; step < numSteps; step++) {
      globals.dispatch(updateStatus(
          'Loading overview ' +
          `${Math.round((step + 1) / numSteps * 1000) / 10}%`));
      const startSec = traceTime.start + step * stepSec;
      const startNs = Math.floor(startSec * 1e9);
      const endSec = startSec + stepSec;
      const endNs = Math.ceil(endSec * 1e9);

      // Sched overview.
      const schedRows = await engine.rawQuery({
        sqlQuery: `select sum(dur)/${stepSec}/1e9, cpu from sched ` +
            `where ts >= ${startNs} and ts < ${endNs} and utid != 0 ` +
            'group by cpu order by cpu'
      });
      const schedData: {[key: string]: QuantizedLoad} = {};
      for (let i = 0; i < schedRows.numRecords; i++) {
        const load = schedRows.columns[0].doubleValues![i];
        const cpu = schedRows.columns[1].longValues![i] as number;
        schedData[cpu] = {startSec, endSec, load};
      }  // for (record ...)
      globals.publish('OverviewData', schedData);

      // Slices overview.
      const slicesRows = await engine.rawQuery({
        sqlQuery:
            `select sum(dur)/${stepSec}/1e9, process.name, process.pid, upid ` +
            'from slices inner join thread using(utid) ' +
            'inner join process using(upid) where depth = 0 ' +
            `and ts >= ${startNs} and ts < ${endNs} ` +
            'group by upid'
      });
      const slicesData: {[key: string]: QuantizedLoad} = {};
      for (let i = 0; i < slicesRows.numRecords; i++) {
        const load = slicesRows.columns[0].doubleValues![i];
        let procName = slicesRows.columns[1].stringValues![i];
        const pid = slicesRows.columns[2].longValues![i];
        procName += ` [${pid}]`;
        slicesData[procName] = {startSec, endSec, load};
      }
      globals.publish('OverviewData', slicesData);
    }  // for (step ...)
  }
}
