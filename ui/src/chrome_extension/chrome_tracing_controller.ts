// Copyright (C) 2019 The Android Open Source Project
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

import {Protocol} from 'devtools-protocol';
import {ProtocolProxyApi} from 'devtools-protocol/types/protocol-proxy-api';
import * as rpc from 'noice-json-rpc';

import {TraceConfig} from '../common/protos';
import {
  ConsumerPortResponse,
  GetTraceStatsResponse,
  ReadBuffersResponse
} from '../controller/consumer_port_types';
import {perfetto} from '../gen/protos';

import {DevToolsSocket} from './devtools_socket';

const CHUNK_SIZE: number = 1024 * 1024 * 64;

export class ChromeTracingController {
  private streamHandle: string|undefined = undefined;
  private uiPort: chrome.runtime.Port;
  private api: ProtocolProxyApi.ProtocolApi;
  private devtoolsSocket: DevToolsSocket;
  private lastBufferUsageEvent: Protocol.Tracing.BufferUsageEvent|undefined;

  constructor(port: chrome.runtime.Port) {
    this.uiPort = port;
    this.devtoolsSocket = new DevToolsSocket();
    this.devtoolsSocket.on('close', () => this.resetState());
    const rpcClient = new rpc.Client(this.devtoolsSocket);
    this.api = rpcClient.api();
    this.api.Tracing.on('tracingComplete', this.onTracingComplete.bind(this));
    this.api.Tracing.on('bufferUsage', this.onBufferUsage.bind(this));
  }

  sendMessage(message: ConsumerPortResponse) {
    this.uiPort.postMessage(message);
  }

  sendErrorMessage(error: string) {
    this.uiPort.postMessage({type: 'ErrorResponse', result: {error}});
  }

  onMessage(request: {method: string, traceConfig: Uint8Array}) {
    switch (request.method) {
      case 'EnableTracing':
        this.enableTracing(request);
        break;
      case 'FreeBuffers':
        this.freeBuffers();
        break;
      case 'ReadBuffers':
        this.readBuffers();
        break;
      case 'DisableTracing':
        this.disableTracing();
        break;
      case 'GetTraceStats':
        this.getTraceStats();
        break;
      default:
        this.sendErrorMessage('Action not recognised');
        console.log('Received not recognized message');
        break;
    }
  }

  enableTracing(request: {method: string, traceConfig: Uint8Array}) {
    this.resetState();
    const traceConfig = TraceConfig.decode(new Uint8Array(request.traceConfig));
    const chromeConfig = this.extractChromeConfig(traceConfig);
    this.handleStartTracing(chromeConfig);
  }

  extractChromeConfig(perfettoConfig: TraceConfig):
      Protocol.Tracing.TraceConfig {
    for (const ds of perfettoConfig.dataSources) {
      if (ds.config && ds.config.name === 'org.chromium.trace_event' &&
          ds.config.chromeConfig && ds.config.chromeConfig.traceConfig) {
        const chromeConfigJsonString = ds.config.chromeConfig.traceConfig;
        return JSON.parse(chromeConfigJsonString) as
            Protocol.Tracing.TraceConfig;
      }
    }
    return {};
  }

  freeBuffers() {
    this.devtoolsSocket.detach();
    this.sendMessage({type: 'FreeBuffersResponse'});
  }

  async readBuffers(offset = 0) {
    // TODO(nicomazz): Add error handling also in the frontend.
    if (!this.devtoolsSocket.isAttached() || this.streamHandle === undefined) {
      this.sendErrorMessage('No tracing session to read from');
      return;
    }

    const res = await this.api.IO.read(
        {handle: this.streamHandle, offset, size: CHUNK_SIZE});
    if (res === undefined) return;

    const chunk = res.base64Encoded ? atob(res.data) : res.data;
    // TODO(nicomazz): remove the conversion to unknown when we stream each
    // chunk to the trace processor.
    const response: ReadBuffersResponse = {
      type: 'ReadBuffersResponse',
      slices:
          [{data: chunk as unknown as Uint8Array, lastSliceForPacket: res.eof}]
    };
    this.sendMessage(response);
    if (res.eof) return;
    this.readBuffers(offset + res.data.length);
  }

  async disableTracing() {
    await this.api.Tracing.end();
    this.sendMessage({type: 'DisableTracingResponse'});
  }

  getTraceStats() {
    let percentFull = 0;  // If the statistics are not available yet, it is 0.
    if (this.lastBufferUsageEvent && this.lastBufferUsageEvent.percentFull) {
      percentFull = this.lastBufferUsageEvent.percentFull;
    }
    const stats: perfetto.protos.ITraceStats = {
      bufferStats:
          [{bufferSize: 1000, bytesWritten: Math.round(percentFull * 1000)}]
    };
    const response: GetTraceStatsResponse = {
      type: 'GetTraceStatsResponse',
      traceStats: stats
    };
    this.sendMessage(response);
  }

  resetState() {
    this.devtoolsSocket.detach();
    this.streamHandle = undefined;
  }

  onTracingComplete(params: Protocol.Tracing.TracingCompleteEvent) {
    this.streamHandle = params.stream;
    this.sendMessage({type: 'EnableTracingResponse'});
  }

  onBufferUsage(params: Protocol.Tracing.BufferUsageEvent) {
    this.lastBufferUsageEvent = params;
  }

  handleStartTracing(traceConfig: Protocol.Tracing.TraceConfig) {
    this.devtoolsSocket.findAndAttachTarget(async _ => {
      await this.api.Tracing.start({
        traceConfig,
        streamFormat: 'proto',
        transferMode: 'ReturnAsStream',
        streamCompression: 'gzip',
        bufferUsageReportingInterval: 200
      });
    });
  }
}
