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
 * A long trace session for local targets (e.g. traced over websocket on the
 * same machine). Sets outputPath so traced writes directly to a known file.
 * After recording stops, the session reports the file path so the user can
 * open it via the UI's "Open trace file" mechanism.
 */
export class LocalLongTraceSession implements TracingSession {
  private consumerIpc: TracingProtocol;
  private _state: TracingSessionState = 'RECORDING';
  readonly logs = new Array<TracingSessionLogEntry>();
  readonly onSessionUpdate = new EvtSource<void>();

  constructor(
    consumerIpc: TracingProtocol,
    traceConfig: protos.ITraceConfig,
    private readonly outputPath: string,
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
    // Data is on the local filesystem; no in-browser data available.
    return undefined;
  }

  private onTraceStopped(error: string) {
    if (error !== '') {
      this.fail(error);
      return;
    }
    if (!['STOPPING', 'RECORDING'].includes(this._state)) return;

    this.log(`Trace saved to ${this.outputPath}`);
    this.consumerIpc.close();
    this.setState('FINISHED');
  }

  private onProtocolClose() {
    if (this._state === 'RECORDING') {
      this.fail('Protocol disconnected');
    }
  }

  private setState(newState: TracingSessionState) {
    this._state = newState;
    this.onSessionUpdate.notify();
  }

  private log(message: string, isError = false) {
    this.logs.push({message, timestamp: new Date(), isError});
    this.onSessionUpdate.notify();
  }

  private fail(error: string) {
    this.log(`Tracing failed: ${error}`, true);
    this.setState('ERRORED');
    this.consumerIpc.close();
  }
}
