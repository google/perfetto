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

import {base64Encode} from '../base/string_utils';
import {
  browserSupportsPerfettoConfig,
  extractTraceConfig,
  hasSystemDataSourceConfig,
} from '../base/trace_config_utils';
import {TraceConfig} from '../common/protos';
import {
  ConsumerPortResponse,
  GetTraceStatsResponse,
  ReadBuffersResponse,
} from '../controller/consumer_port_types';
import {RpcConsumerPort} from '../controller/record_controller_interfaces';
import {perfetto} from '../gen/protos';

import {DevToolsSocket} from './devtools_socket';

const CHUNK_SIZE: number = 1024 * 1024 * 16;  // 16Mb

export class ChromeTracingController extends RpcConsumerPort {
  private streamHandle: string|undefined = undefined;
  private uiPort: chrome.runtime.Port;
  private api: ProtocolProxyApi.ProtocolApi;
  private devtoolsSocket: DevToolsSocket;
  private lastBufferUsageEvent: Protocol.Tracing.BufferUsageEvent|undefined;
  private tracingSessionOngoing = false;
  private tracingSessionId = 0;

  constructor(port: chrome.runtime.Port) {
    super({
      onConsumerPortResponse: (message: ConsumerPortResponse) =>
          this.uiPort.postMessage(message),

      onError: (error: string) =>
          this.uiPort.postMessage({type: 'ChromeExtensionError', error}),

      onStatus: (status) =>
          this.uiPort.postMessage({type: 'ChromeExtensionStatus', status}),
    });
    this.uiPort = port;
    this.devtoolsSocket = new DevToolsSocket();
    this.devtoolsSocket.on('close', () => this.resetState());
    const rpcClient = new rpc.Client(this.devtoolsSocket);
    this.api = rpcClient.api();
    this.api.Tracing.on('tracingComplete', this.onTracingComplete.bind(this));
    this.api.Tracing.on('bufferUsage', this.onBufferUsage.bind(this));
    this.uiPort.onDisconnect.addListener(() => {
      this.devtoolsSocket.detach();
    });
  }

  handleCommand(methodName: string, requestData: Uint8Array) {
    switch (methodName) {
      case 'EnableTracing':
        this.enableTracing(requestData);
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
      case 'GetCategories':
        this.getCategories();
        break;
      default:
        this.sendErrorMessage('Action not recognized');
        console.log('Received not recognized message: ', methodName);
        break;
    }
  }

  enableTracing(enableTracingRequest: Uint8Array) {
    this.resetState();
    const traceConfigProto = extractTraceConfig(enableTracingRequest);
    if (!traceConfigProto) {
      this.sendErrorMessage('Invalid trace config');
      return;
    }

    this.handleStartTracing(traceConfigProto);
  }

  toCamelCase(key: string, separator: string): string {
    return key.split(separator)
        .map((part, index) => {
          return (index === 0) ? part : part[0].toUpperCase() + part.slice(1);
        })
        .join('');
  }

  convertDictKeys(obj: any): any {
    if (Array.isArray(obj)) {
      return obj.map((v) => this.convertDictKeys(v));
    }
    if (typeof obj === 'object' && obj !== null) {
      const converted: any = {};
      for (const key of Object.keys(obj)) {
        converted[this.toCamelCase(key, '_')] = this.convertDictKeys(obj[key]);
      }
      return converted;
    }
    return obj;
  }

  convertToDevToolsConfig(config: any): Protocol.Tracing.TraceConfig {
    // DevTools uses a different naming style for config properties: Dictionary
    // keys are named "camelCase" style, rather than "underscore_case" style as
    // in the TraceConfig.
    config = this.convertDictKeys(config);
    // recordMode is specified as an enum with camelCase values.
    if (config.recordMode) {
      config.recordMode = this.toCamelCase(config.recordMode as string, '-');
    }
    return config as Protocol.Tracing.TraceConfig;
  }

  // TODO(nicomazz): write unit test for this
  extractChromeConfig(perfettoConfig: TraceConfig):
      Protocol.Tracing.TraceConfig {
    for (const ds of perfettoConfig.dataSources) {
      if (ds.config && ds.config.name === 'org.chromium.trace_event' &&
          ds.config.chromeConfig && ds.config.chromeConfig.traceConfig) {
        const chromeConfigJsonString = ds.config.chromeConfig.traceConfig;
        const config = JSON.parse(chromeConfigJsonString);
        return this.convertToDevToolsConfig(config);
      }
    }
    return {};
  }

  freeBuffers() {
    this.devtoolsSocket.detach();
    this.sendMessage({type: 'FreeBuffersResponse'});
  }

  async readBuffers(offset = 0) {
    if (!this.devtoolsSocket.isAttached() || this.streamHandle === undefined) {
      this.sendErrorMessage('No tracing session to read from');
      return;
    }

    const res = await this.api.IO.read(
        {handle: this.streamHandle, offset, size: CHUNK_SIZE});
    if (res === undefined) return;

    const chunk = res.base64Encoded ? atob(res.data) : res.data;
    // The 'as {} as UInt8Array' is done because we can't send ArrayBuffers
    // trough a chrome.runtime.Port. The conversion from string to ArrayBuffer
    // takes place on the other side of the port.
    const response: ReadBuffersResponse = {
      type: 'ReadBuffersResponse',
      slices: [{data: chunk as {} as Uint8Array, lastSliceForPacket: res.eof}],
    };
    this.sendMessage(response);
    if (res.eof) return;
    this.readBuffers(offset + res.data.length);
  }

  async disableTracing() {
    await this.endTracing(this.tracingSessionId);
    this.sendMessage({type: 'DisableTracingResponse'});
  }

  async endTracing(tracingSessionId: number) {
    if (tracingSessionId !== this.tracingSessionId) {
      return;
    }
    if (this.tracingSessionOngoing) {
      await this.api.Tracing.end();
    }
    this.tracingSessionOngoing = false;
  }

  getTraceStats() {
    let percentFull = 0;  // If the statistics are not available yet, it is 0.
    if (this.lastBufferUsageEvent && this.lastBufferUsageEvent.percentFull) {
      percentFull = this.lastBufferUsageEvent.percentFull;
    }
    const stats: perfetto.protos.ITraceStats = {
      bufferStats:
          [{bufferSize: 1000, bytesWritten: Math.round(percentFull * 1000)}],
    };
    const response: GetTraceStatsResponse = {
      type: 'GetTraceStatsResponse',
      traceStats: stats,
    };
    this.sendMessage(response);
  }

  getCategories() {
    const fetchCategories = async () => {
      const categories = (await this.api.Tracing.getCategories()).categories;
      this.uiPort.postMessage({type: 'GetCategoriesResponse', categories});
    };
    // If a target is already attached, we simply fetch the categories.
    if (this.devtoolsSocket.isAttached()) {
      fetchCategories();
      return;
    }
    // Otherwise, we attach temporarily.
    this.devtoolsSocket.attachToBrowser(async (error?: string) => {
      if (error) {
        this.sendErrorMessage(
            `Could not attach to DevTools browser target ` +
            `(req. Chrome >= M81): ${error}`);
        return;
      }
      fetchCategories();
      this.devtoolsSocket.detach();
    });
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

  handleStartTracing(traceConfigProto: Uint8Array) {
    this.devtoolsSocket.attachToBrowser(async (error?: string) => {
      if (error) {
        this.sendErrorMessage(
            `Could not attach to DevTools browser target ` +
            `(req. Chrome >= M81): ${error}`);
        return;
      }

      const requestParams: Protocol.Tracing.StartRequest = {
        streamFormat: 'proto',
        transferMode: 'ReturnAsStream',
        streamCompression: 'gzip',
        bufferUsageReportingInterval: 200,
      };

      const traceConfig = TraceConfig.decode(traceConfigProto);
      if (browserSupportsPerfettoConfig()) {
        const configEncoded = base64Encode(traceConfigProto);
        await this.api.Tracing.start(
            {perfettoConfig: configEncoded, ...requestParams});
        this.tracingSessionOngoing = true;
        const tracingSessionId = ++this.tracingSessionId;
        setTimeout(
            () => this.endTracing(tracingSessionId), traceConfig.durationMs);
      } else {
        console.log(
            'Used Chrome version is too old to support ' +
            'perfettoConfig parameter. Using chrome config only instead.');

        if (hasSystemDataSourceConfig(traceConfig)) {
          this.sendErrorMessage(
              'System tracing is not supported by this Chrome version. Choose' +
              ' the \'Chrome\' target instead to record a Chrome-only trace.');
          return;
        }

        const chromeConfig = this.extractChromeConfig(traceConfig);
        await this.api.Tracing.start(
            {traceConfig: chromeConfig, ...requestParams});
      }
    });
  }
}
