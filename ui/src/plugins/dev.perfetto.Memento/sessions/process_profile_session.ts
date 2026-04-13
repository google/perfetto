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
import {AdbDevice} from '../../dev.perfetto.RecordTraceV2/adb/adb_device';
import {createAdbTracingSession} from '../../dev.perfetto.RecordTraceV2/adb/adb_tracing_session';
import {TracedWebsocketTarget} from '../../dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';

export type ProfileState = 'recording' | 'stopping' | 'finished' | 'error';

/**
 * Builds the TraceConfig for a single-process heap profiling session.
 * Uses three buffers: heap profiling (0), Java HPROF (1), process stats (2).
 */
function buildProcessProfileConfig(pid: number): protos.ITraceConfig {
  return {
    buffers: [
      {
        // Process stats buffer
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.DISCARD,
      },
      {
        // Heapprofd buffer
        sizeKb: 128 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
      {
        // Java HPROF buffer
        sizeKb: 512 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
    ],
    dataSources: [
      {
        config: {
          name: 'linux.process_stats',
          targetBuffer: 0,
          processStatsConfig: {
            scanAllProcessesOnStart: true,
          },
        },
      },
      {
        config: {
          name: 'android.heapprofd',
          targetBuffer: 1,
          heapprofdConfig: {
            pid: [pid],
            samplingIntervalBytes: 4096,
            allHeaps: true,
            shmemSizeBytes: 16 * 1024 * 1024,
          },
        },
      },
      {
        config: {
          name: 'android.java_hprof',
          targetBuffer: 2,
          javaHprofConfig: {
            pid: [pid],
            continuousDumpConfig: {
              dumpIntervalMs: 10 * 1000,
            },
          },
        },
      },
    ],
  };
}

const STATS_POLL_INTERVAL_MS = 3000;

export interface ProcessProfileSession {
  readonly pid: number;
  readonly processName: string;
  readonly startX: number;
  readonly state: ProfileState;
  readonly error?: string;
  readonly bufferUsagePct?: number;
  /** Stops recording and waits for the trace data to be ready. */
  stop(): Promise<void>;
  /** Cancels recording and discards trace data. */
  cancel(): Promise<void>;
  /** Returns the trace buffer once state is 'finished'. */
  getTraceData(): Uint8Array | undefined;
}

export async function createProcessProfileSession(
  targetOrDevice: TracedWebsocketTarget | AdbDevice,
  pid: number,
  processName: string,
  startX: number,
): Promise<ProcessProfileSession> {
  const config = buildProcessProfileConfig(pid);
  const result =
    targetOrDevice instanceof TracedWebsocketTarget
      ? await targetOrDevice.startTracing(config)
      : await createAdbTracingSession(targetOrDevice, config);
  if (!result.ok) {
    // TODO: Put this in the error state of the returned object
    return {
      state: 'error' as ProfileState,
      error: `Failed to start profile: ${result.error}`,
      startX,
      pid,
      processName,
      async stop() {},
      async cancel() {},
      getTraceData() {
        return undefined;
      },
    };
  }

  let bufferUsagePct: number | undefined;
  const session = result.value;
  let state: ProfileState = 'recording';
  let error: string | undefined = undefined;

  const intervalHandle = setInterval(async () => {
    bufferUsagePct = await session.getBufferUsagePct();
  }, STATS_POLL_INTERVAL_MS);

  session.onSessionUpdate.addListener(() => {
    if (session.state === 'FINISHED') {
      state = 'finished';
    } else if (session.state === 'ERRORED') {
      state = 'error';
      error = session.logs
        .filter((l) => l.isError)
        .map((l) => l.message)
        .join('; ');
    }
  });

  return {
    pid,
    processName,
    startX,
    state,
    get bufferUsagePct() {
      return bufferUsagePct;
    },
    /** Stops recording and waits for the trace data to be ready. */
    async stop() {
      clearInterval(intervalHandle);
      state = 'stopping';
      await session.stop();
      // Wait for the session to reach FINISHED if it hasn't already.
      if (session.state !== 'FINISHED') {
        await new Promise<void>((resolve) => {
          const sub = session.onSessionUpdate.addListener(() => {
            if (session.state === 'FINISHED' || session.state === 'ERRORED') {
              sub[Symbol.dispose]();
              resolve();
            }
          });
        });
      }
      state = session.state === 'FINISHED' ? 'finished' : 'error';
    },
    async cancel(): Promise<void> {
      await session.cancel();
      state = 'error';
      clearInterval(intervalHandle);
    },
    /** Returns the trace buffer once state is 'finished'. */
    getTraceData(): Uint8Array | undefined {
      return session.getTraceData();
    },
  };
}
