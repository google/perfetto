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

import protos from '../../protos';
import {TracingSession} from '../dev.perfetto.RecordTraceV2/interfaces/tracing_session';

export type ProfileState = 'recording' | 'stopping' | 'finished' | 'error';

const heapProfBufferSizeKb = 128 * 1024;

/**
 * Builds the TraceConfig for a single-process heap profiling session.
 * Uses three buffers: heap profiling (0), Java HPROF (1), process stats (2).
 */
export function buildProcessProfileConfig(pid: number): protos.ITraceConfig {
  return {
    buffers: [
      {
        sizeKb: heapProfBufferSizeKb,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
      {
        sizeKb: heapProfBufferSizeKb,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
      {
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
    ],
    dataSources: [
      {
        config: {
          name: 'android.heapprofd',
          targetBuffer: 0,
          heapprofdConfig: {
            pid: [pid],
            samplingIntervalBytes: 4096,
            allHeaps: true,
            shmemSizeBytes: 8 * 1024 * 1024,
          },
        },
      },
      {
        config: {
          name: 'android.java_hprof',
          targetBuffer: 1,
          javaHprofConfig: {
            pid: [pid],
            continuousDumpConfig: {
              dumpIntervalMs: 1000,
            },
          },
        },
      },
      {
        config: {
          name: 'linux.process_stats',
          targetBuffer: 2,
          processStatsConfig: {
            scanAllProcessesOnStart: true,
          },
        },
      },
    ],
  };
}

/**
 * Handle to a single-process heap profiling session. Call stop() to end
 * recording and pull the trace buffer, then pass the result to
 * app.openTraceFromBuffer() to view in the main UI.
 */
export class ProcessProfile {
  private readonly session: TracingSession;
  readonly pid: number;
  readonly processName: string;
  state: ProfileState = 'recording';
  error?: string;

  constructor(session: TracingSession, pid: number, processName: string) {
    this.session = session;
    this.pid = pid;
    this.processName = processName;
    this.session.onSessionUpdate.addListener(() => {
      if (this.session.state === 'FINISHED') {
        this.state = 'finished';
      } else if (this.session.state === 'ERRORED') {
        this.state = 'error';
        this.error = this.session.logs
          .filter((l) => l.isError)
          .map((l) => l.message)
          .join('; ');
      }
    });
  }

  /** Stops recording and waits for the trace data to be ready. */
  async stop(): Promise<void> {
    this.state = 'stopping';
    await this.session.stop();
    // Wait for the session to reach FINISHED if it hasn't already.
    if (this.session.state !== 'FINISHED') {
      await new Promise<void>((resolve) => {
        const sub = this.session.onSessionUpdate.addListener(() => {
          if (
            this.session.state === 'FINISHED' ||
            this.session.state === 'ERRORED'
          ) {
            sub[Symbol.dispose]();
            resolve();
          }
        });
      });
    }
    this.state = this.session.state === 'FINISHED' ? 'finished' : 'error';
  }

  /** Cancels recording and discards trace data. */
  async cancel(): Promise<void> {
    await this.session.cancel();
    this.state = 'error';
  }

  /** Returns the trace buffer once state is 'finished'. */
  getTraceData(): Uint8Array | undefined {
    return this.session.getTraceData();
  }
}
