// Copyright (C) 2025 The Android Open Source Project
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
import {EvtSource} from '../../../base/events';
import {
  RecordingProgress,
  TraceData,
  TracingSession,
  TracingSessionLogEntry,
  TracingSessionState,
} from '../interfaces/tracing_session';
import {TracingProtocol} from './tracing_protocol';

/**
 * Interface for retrieving trace files from a target device by streaming
 * them into a writable file on the host.
 */
export interface FileRetriever {
  pullFile(
    path: string,
    writable: FileSystemWritableFileStream,
    onProgress: (pct: number) => void,
  ): Promise<{ok: true} | {ok: false; error: string}>;
  deleteFile(path: string): Promise<void>;
}

/**
 * A variant of ConsumerIpcTracingSession for write_into_file (long trace) mode.
 *
 * Instead of calling ReadBuffers after tracing stops, this session:
 * 1. Sets outputPath in the trace config so traced writes directly to a file
 * 2. After tracing stops, transitions to PULLING state and uses a FileRetriever
 *    to stream the file into a local File (via FileSystemFileHandle)
 * 3. On success, stores the File and transitions to FINISHED
 * 4. On failure, transitions to ERRORED with the device file path so the user
 *    can pull manually
 */
export class LongTraceTracingSession implements TracingSession {
  private consumerIpc: TracingProtocol;
  private _state: TracingSessionState = 'RECORDING';
  private traceFile?: File;
  readonly logs = new Array<TracingSessionLogEntry>();
  readonly onSessionUpdate = new EvtSource<void>();

  constructor(
    consumerIpc: TracingProtocol,
    traceConfig: protos.ITraceConfig,
    private readonly outputPath: string,
    private readonly fileRetriever: FileRetriever,
    private readonly fileHandle: FileSystemFileHandle,
  ) {
    this.consumerIpc = consumerIpc;
    this.consumerIpc.onClose = this.onProtocolClose.bind(this);
    this.start(traceConfig);
  }

  get state(): TracingSessionState {
    return this._state;
  }

  private async start(traceConfig: protos.ITraceConfig): Promise<void> {
    const cfg = {...traceConfig, outputPath: this.outputPath};
    const req = new protos.EnableTracingRequest({traceConfig: cfg});
    this.log(
      `Starting long trace, durationMs: ${traceConfig.durationMs}, ` +
        `outputPath: ${this.outputPath}`,
    );
    const resp = await this.consumerIpc.invoke('EnableTracing', req);
    this.onTraceStopped(resp.error);
  }

  async stop(): Promise<void> {
    if (this._state !== 'RECORDING') return;
    this.setState('STOPPING');
    // Initiator=kPerfettoCmd, Reason=kTraceStop. See flush_flags.h.
    const flags = (2 << 4) | 2;
    this.log('Flushing data sources');
    await this.consumerIpc.invoke('Flush', new protos.FlushRequest({flags}));
    this.log('Flush complete, stopping trace');
    const disReq = new protos.DisableTracingRequest({});
    await this.consumerIpc.invoke('DisableTracing', disReq);
  }

  async cancel(): Promise<void> {
    if (!['RECORDING', 'STOPPING'].includes(this._state)) return;
    const req = new protos.FreeBuffersRequest({});
    await this.consumerIpc.invoke('FreeBuffers', req);
    this.fail('Trace cancelled');
  }

  async getRecordingProgress(): Promise<RecordingProgress | undefined> {
    if (this._state !== 'RECORDING') return undefined;
    const req = new protos.GetTraceStatsRequest({});
    const resp = await this.consumerIpc.invoke('GetTraceStats', req);
    let totalBytesWritten = 0;
    for (const buf of resp.traceStats?.bufferStats ?? []) {
      totalBytesWritten += buf.bytesWritten ?? 0;
    }
    return {kind: 'TOTAL_WRITTEN', bytes: totalBytesWritten};
  }

  getTraceData(): TraceData | undefined {
    if (this._state !== 'FINISHED' || this.traceFile === undefined) {
      return undefined;
    }
    return {kind: 'FILE', file: this.traceFile, outputPath: this.outputPath};
  }

  private async onTraceStopped(error: string): Promise<void> {
    if (error !== '') {
      this.fail(error);
      return;
    }
    if (!['STOPPING', 'RECORDING'].includes(this._state)) return;

    this.setState('PULLING');
    this.log(`Tracing stopped. Pulling trace from ${this.outputPath}`);
    this.consumerIpc.close();

    let lastLoggedPct = 0;
    const writable = await this.fileHandle.createWritable();
    const result = await this.fileRetriever.pullFile(
      this.outputPath,
      writable,
      (pct) => {
        const bucket = Math.floor(pct / 20) * 20;
        if (bucket > lastLoggedPct) {
          lastLoggedPct = bucket;
          this.log(`Pulling trace: ${bucket}%`);
        }
      },
    );
    await writable.close();

    if (!result.ok) {
      this.log(
        `Failed to pull trace file: ${result.error}. ` +
          `You can pull it manually: adb pull ${this.outputPath}`,
        true,
      );
      this.setState('ERRORED');
      return;
    }

    this.traceFile = await this.fileHandle.getFile();
    this.log(`Trace file pulled successfully`);

    // Clean up the file on the device.
    this.fileRetriever.deleteFile(this.outputPath).catch(() => {}); // Best-effort cleanup.

    this.setState('FINISHED');
  }

  private onProtocolClose() {
    // In long trace mode, we close the protocol ourselves after tracing stops
    // (before pulling the file). Only treat unexpected closures as errors.
    if (this._state === 'RECORDING') {
      this.fail('Protocol disconnected');
    }
  }

  private setState(newState: TracingSessionState) {
    this._state = newState;
    this.onSessionUpdate.notify();
  }

  private log(message: string, isError = false) {
    this.logs.push({
      message,
      timestamp: new Date(),
      isError,
    });
    this.onSessionUpdate.notify();
  }

  private fail(error: string) {
    this.log(`Tracing failed: ${error}`, true);
    this.setState('ERRORED');
    this.consumerIpc.close();
  }
}
