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

import protos from '../../../protos';
import type {AdbDevice} from '../../dev.perfetto.RecordTraceV2/adb/adb_device';
import {createAdbTracingSession} from '../../dev.perfetto.RecordTraceV2/adb/adb_tracing_session';
import type {TracingSession} from '../../dev.perfetto.RecordTraceV2/interfaces/tracing_session';
import {traceConfigToPb} from '../../../base/proto_utils_wasm';
import {buildRecordConfigTxt} from '../product_assets';
import {TracedWebsocketTarget} from '../../dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';

const DUMP_INTERVAL_MS = 10_000;
const STATS_POLL_INTERVAL_MS = 3_000;

export type ProfileState = 'recording' | 'stopping' | 'finished' | 'error';

export class ProfileSession {
  readonly pid: number;
  readonly processName: string;

  private inner?: TracingSession;
  private intervalHandle?: ReturnType<typeof setInterval>;
  private _state: ProfileState = 'recording';
  private _error?: string;
  private _bufferUsagePct?: number;

  private constructor(pid: number, processName: string) {
    this.pid = pid;
    this.processName = processName;
  }

  static async start(
    targetOrDevice: TracedWebsocketTarget | AdbDevice,
    pid: number,
    processName: string,
  ): Promise<ProfileSession> {
    const self = new ProfileSession(pid, processName);
    const config = await buildProcessProfileConfig(pid, processName);
    const result =
      targetOrDevice instanceof TracedWebsocketTarget
        ? await targetOrDevice.startTracing(config)
        : await createAdbTracingSession(targetOrDevice, config);
    if (!result.ok) {
      self._state = 'error';
      self._error = `Failed to start profile: ${result.error}`;
      return self;
    }
    self.inner = result.value;
    self.intervalHandle = setInterval(async () => {
      self._bufferUsagePct = await self.inner!.getBufferUsagePct();
    }, STATS_POLL_INTERVAL_MS);
    self.inner.onSessionUpdate.addListener(() => {
      const s = self.inner!.state;
      if (s === 'FINISHED') {
        self._state = 'finished';
      } else if (s === 'ERRORED') {
        self._state = 'error';
        self._error = self
          .inner!.logs.filter((l) => l.isError)
          .map((l) => l.message)
          .join('; ');
      }
    });
    return self;
  }

  get state(): ProfileState {
    return this._state;
  }

  get error(): string | undefined {
    return this._error;
  }

  get bufferUsagePct(): number | undefined {
    return this._bufferUsagePct;
  }

  /** Stops recording and waits for the trace data to be ready. */
  async stop(): Promise<void> {
    if (this._state !== 'recording' || this.inner === undefined) return;
    clearInterval(this.intervalHandle);
    this._state = 'stopping';
    await this.inner.stop();
    if (this.inner.state !== 'FINISHED') {
      await new Promise<void>((resolve) => {
        const sub = this.inner!.onSessionUpdate.addListener(() => {
          const s = this.inner!.state;
          if (s === 'FINISHED' || s === 'ERRORED') {
            sub[Symbol.dispose]();
            resolve();
          }
        });
      });
    }
    this._state = this.inner.state === 'FINISHED' ? 'finished' : 'error';
  }

  /** Cancels recording and discards trace data. */
  async cancel(): Promise<void> {
    if (this._state !== 'recording' || this.inner === undefined) return;
    clearInterval(this.intervalHandle);
    this._state = 'error';
    await this.inner.cancel();
  }

  /** Returns the trace buffer once state is 'finished'. */
  getTraceData(): ArrayBuffer | undefined {
    return this.inner?.getTraceData();
  }
}

// The shared single-app config (products/memscope/configs/record.pbtx),
// targeted at |pid| and running until stopped.
async function buildProcessProfileConfig(
  pid: number,
  processName: string,
): Promise<protos.ITraceConfig> {
  const txt = buildRecordConfigTxt({
    app: processName,
    durationMs: 0,
    javaDumps: {phaseMs: 0, intervalMs: DUMP_INTERVAL_MS},
  });
  const pb = await traceConfigToPb(txt);
  if (!pb.ok) throw new Error(`Bad memscope record config: ${pb.error}`);
  const config = protos.TraceConfig.decode(pb.value);
  config.compressionType =
    protos.TraceConfig.CompressionType.COMPRESSION_TYPE_DEFLATE;
  // Target the exact process picked in the UI rather than every process
  // sharing its name.
  for (const ds of config.dataSources) {
    const c = ds.config;
    for (const target of [c?.heapprofdConfig, c?.javaHprofConfig]) {
      if (target) {
        target.processCmdline = [];
        target.pid = [pid];
      }
    }
  }
  return config;
}
