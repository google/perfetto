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
import {TracedWebsocketTarget} from '../../dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';

const DUMP_INTERVAL_MS = 10_000;
const PROC_STATS_BUFFER_SIZE_KB = 4 * 1024;
const HEAPPROFD_BUFFER_SIZE_KB = 128 * 1024;
const JAVA_HPROF_BUFFER_SIZE_KB = 256 * 1024;
const STATS_POLL_INTERVAL_MS = 3000;

export type ProfileState = 'recording' | 'stopping' | 'finished' | 'error';

export class ProfileSession {
  readonly pid: number;
  readonly processName: string;
  readonly startX: number;

  private inner?: TracingSession;
  private intervalHandle?: ReturnType<typeof setInterval>;
  private _state: ProfileState = 'recording';
  private _error?: string;
  private _bufferUsagePct?: number;

  private constructor(pid: number, processName: string, startX: number) {
    this.pid = pid;
    this.processName = processName;
    this.startX = startX;
  }

  static async start(
    targetOrDevice: TracedWebsocketTarget | AdbDevice,
    pid: number,
    processName: string,
    startX: number,
  ): Promise<ProfileSession> {
    const self = new ProfileSession(pid, processName, startX);
    const config = buildProcessProfileConfig(pid);
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
  getTraceData(): Uint8Array | undefined {
    return this.inner?.getTraceData();
  }
}

function buildProcessProfileConfig(pid: number): protos.ITraceConfig {
  return {
    compressionType:
      protos.TraceConfig.CompressionType.COMPRESSION_TYPE_DEFLATE,
    buffers: [
      {
        name: 'process_stats',
        sizeKb: PROC_STATS_BUFFER_SIZE_KB,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.DISCARD,
      },
      {
        name: 'heapprofd',
        sizeKb: HEAPPROFD_BUFFER_SIZE_KB,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
      {
        name: 'java_hprof',
        sizeKb: JAVA_HPROF_BUFFER_SIZE_KB,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
    ],
    dataSources: [
      {
        config: {
          name: 'linux.process_stats',
          targetBufferName: 'process_stats',
          processStatsConfig: {
            scanAllProcessesOnStart: true, // Necessary for track names.
          },
        },
      },
      {
        config: {
          name: 'android.heapprofd',
          targetBufferName: 'heapprofd',
          heapprofdConfig: {
            pid: [pid],
            samplingIntervalBytes: 32 * 1024, // Slightly larger than default to reduce overhead.
            shmemSizeBytes: 16 * 1024 * 1024,
            blockClient: true, // Important for trace integrity.
            continuousDumpConfig: {
              dumpIntervalMs: DUMP_INTERVAL_MS, // Important for getting regular heap snapshots to see how memory usage evolves over time.
            },
          },
        },
      },
      {
        config: {
          name: 'android.java_hprof',
          targetBufferName: 'java_hprof',
          javaHprofConfig: {
            pid: [pid],
            continuousDumpConfig: {
              dumpIntervalMs: DUMP_INTERVAL_MS, // Required for Java profiles.
            },
          },
        },
      },
    ],
  };
}
