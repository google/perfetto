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
import {binaryDecode} from '../../../base/string_utils';
import {
  TracingSession,
  TracingSessionLogEntry,
  TracingSessionState,
} from '../interfaces/tracing_session';
import {ChromeExtensionTarget} from './chrome_extension_target';
import {defer, Deferred} from '../../../base/deferred';

export class ChromeExtensionTracingSession implements TracingSession {
  private _state: TracingSessionState = 'RECORDING';
  readonly logs = new Array<TracingSessionLogEntry>();
  private traceBuf = new ResizableArrayBuffer(64 * 1024);
  readonly onSessionUpdate = new EvtSource<void>();
  private pendingBufferUsage = new Array<Deferred<number>>();

  constructor(
    private target: ChromeExtensionTarget,
    traceConfig: protos.ITraceConfig,
  ) {
    this.start(traceConfig);
  }

  private async start(traceConfig: protos.ITraceConfig): Promise<void> {
    const requestData = protos.EnableTracingRequest.encode({
      traceConfig,
    }).finish();
    this.target.invokeExtensionMethod('EnableTracing', requestData);
  }

  async stop(): Promise<void> {
    this.target.invokeExtensionMethod('DisableTracing');
    this.setState('STOPPING');
  }

  async cancel(): Promise<void> {
    this.target.invokeExtensionMethod('FreeBuffers');
    this.setState('STOPPING');
  }

  async getBufferUsagePct(): Promise<number | undefined> {
    if (this._state !== 'RECORDING') return undefined;
    const promise = defer<number>();
    this.pendingBufferUsage.push(promise);
    this.target.invokeExtensionMethod('GetTraceStats');
    return promise;
  }

  getTraceData(): Uint8Array | undefined {
    if (this._state !== 'FINISHED') return undefined;
    const buf = this.traceBuf.get();
    return buf;
  }

  onExtensionMessage(msgType: string, msg: object) {
    switch (msgType) {
      case 'ChromeExtensionError':
        const err = (msg as {type: string; error: string}).error;
        this.log(`Tracing failed: ${err}`, /* isError */ true);
        if (this._state !== 'FINISHED') {
          // Ignore spurious errors that arrive after the session finishes.
          this.setState('ERRORED');
          this.target.disconnect();
        }
        break;

      case 'ChromeExtensionStatus':
        const status = (msg as {type: string; status: string}).status;
        this.log(status);
        break;

      case 'EnableTracingResponse':
        this.target.invokeExtensionMethod('ReadBuffers');
        this.setState('STOPPING');
        break;

      case 'GetTraceStatsResponse':
        const statResp = msg as {type: string} & protos.IGetTraceStatsResponse;
        let totSize = 0;
        let usedSize = 0;
        for (const buf of statResp.traceStats?.bufferStats ?? []) {
          totSize += buf.bufferSize ?? 0;
          // bytesWritten can be >> bufferSize for ring buffer traces.
          usedSize += Math.min(buf.bytesWritten ?? 0, buf.bufferSize ?? 0);
        }
        const pct = Math.min(Math.round((100 * usedSize) / totSize), 100);
        for (const promise of this.pendingBufferUsage.splice(0)) {
          promise.resolve(pct);
        }
        break;

      case 'ReadBuffersResponse':
        // The extension is really misusing the ReadBuffersResponse:
        // - Data is a binary string, not a Uint8Array
        // - The field 'lastSliceForPacket' is really 'lastPacketInTrace'.
        // - Slices are really packets and don't need preambles.
        // See http://shortn/_53WB8A1aIr.
        const resp = msg as {type: string} & protos.IReadBuffersResponse;
        let eof = false;
        for (const slice of resp.slices ?? []) {
          const data = binaryDecode(slice.data as unknown as string);
          this.traceBuf.append(data);
          eof = Boolean(slice.lastSliceForPacket);
          if (eof) {
            this.setState('FINISHED');
            this.target.invokeExtensionMethod('FreeBuffers');
            break;
          }
        }
        break;
    }
  }

  get state(): TracingSessionState {
    return this._state;
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
}
