// Copyright (C) 2024 The Android Open Source Project
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
import {ResizableArrayBuffer} from '../../../base/resizable_array_buffer';
import {
  TracingSession,
  TracingSessionLogEntry,
  TracingSessionState,
} from '../interfaces/tracing_session';
import {TracingProtocol} from './tracing_protocol';

/**
 * A concrete implementation of {@link TracingSession} over a
 * Perfetto IPC Tracing Procol. This class is suitable for all cases where we
 * are able to obtain, in a way or another, a byte stream to talk to the traced
 * consumer socket.
 */
export class ConsumerIpcTracingSession implements TracingSession {
  private consumerIpc: TracingProtocol;
  private _state: TracingSessionState = 'RECORDING';
  readonly logs = new Array<TracingSessionLogEntry>();
  private traceBuf = new ResizableArrayBuffer(64 * 1024);
  readonly onSessionUpdate = new EvtSource<void>();

  constructor(consumerIpc: TracingProtocol, traceConfig: protos.ITraceConfig) {
    this.consumerIpc = consumerIpc;
    this.consumerIpc.onClose = this.onProtocolClose.bind(this);
    this.start(traceConfig);
  }

  get state(): TracingSessionState {
    return this._state;
  }

  private async start(traceConfig: protos.ITraceConfig): Promise<void> {
    const req = new protos.EnableTracingRequest({traceConfig});
    this.log(`Starting trace, durationMs: ${traceConfig.durationMs}`);
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

  async getBufferUsagePct(): Promise<number | undefined> {
    if (this._state !== 'RECORDING') return undefined;
    const req = new protos.GetTraceStatsRequest({});
    const resp = await this.consumerIpc.invoke('GetTraceStats', req);
    let totSize = 0;
    let usedSize = 0;
    for (const buf of resp.traceStats?.bufferStats ?? []) {
      totSize += buf.bufferSize ?? 0;
      // bytesWritten can be >> bufferSize for ring buffer traces.
      usedSize += Math.min(buf.bytesWritten ?? 0, buf.bufferSize ?? 0);
    }
    return Math.min(Math.round((100 * usedSize) / totSize), 100);
  }

  private onTraceStopped(error: string) {
    if (error !== '') {
      this.fail(error);
      return;
    }
    if (this.consumerIpc === undefined) {
      return; // Spurious event after we failed.
    }
    // There is nothing more to do if we arrive here via cancel() or an error.
    if (!['STOPPING', 'RECORDING'].includes(this._state)) return;

    // We reach this point either:
    // 1. In state == 'RECORDING', if the durationMs expired and the
    //    EnableTracing request is resolved.
    // 2. In state == 'STOPPING', if the user has pressed stop().
    this.setState('STOPPING');
    this.log('Tracing stopped. Reading back data');
    const rbreq = new protos.ReadBuffersRequest({});
    const stream = this.consumerIpc.invokeStreaming('ReadBuffers', rbreq);
    stream.onTraceData = this.onTraceData.bind(this);
  }

  getTraceData(): Uint8Array | undefined {
    if (this._state !== 'FINISHED') return undefined;
    const buf = this.traceBuf.get();
    return buf;
  }

  private onTraceData(packets: Uint8Array, hasMore: boolean) {
    this.traceBuf.append(packets);
    if (hasMore) return;

    this.setState('FINISHED');
    this.consumerIpc?.close();
  }

  private onProtocolClose() {
    if (this._state === 'RECORDING') {
      this.setState('ERRORED');
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

  fail(error: string) {
    this.log(`Tracing failed: ${error}`, /* isError */ true);
    this.setState('ERRORED');
    this.consumerIpc.close();
  }
}
