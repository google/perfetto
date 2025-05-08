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
import {errResult, okResult, Result} from '../../../base/result';
import {RecordingTarget} from '../interfaces/recording_target';
import {PreflightCheck} from '../interfaces/connection_check';
import {AsyncWebsocket} from '../websocket/async_websocket';
import {websocketInstructions} from '../websocket/websocket_utils';
import {ConsumerIpcTracingSession} from '../tracing_protocol/consumer_ipc_tracing_session';
import {WebSocketStream} from '../websocket/websocket_stream';
import {TracingProtocol} from '../tracing_protocol/tracing_protocol';
import {exists} from '../../../base/utils';
import {AsyncLazy} from '../../../base/async_lazy';

export class TracedWebsocketTarget implements RecordingTarget {
  readonly kind = 'LIVE_RECORDING';
  readonly platform = 'LINUX';
  readonly transportType = 'WebSocket';

  // This Consumer connection is only used to detect the connection state and
  // to query servce state. each new tracing session creates a new instance,
  // because consumer connections in traced are single-use.
  private mgmtConsumer = new AsyncLazy<TracingProtocol>();

  /**
   * @param wsUrl 'ws://127.0.0.1:8037/traced'
   */
  constructor(readonly wsUrl: string) {}

  get id(): string {
    return this.wsUrl;
  }

  get name(): string {
    return this.wsUrl;
  }

  get connected(): boolean {
    return this.mgmtConsumer.value?.connected ?? false;
  }

  async *runPreflightChecks(): AsyncGenerator<PreflightCheck> {
    const status = await this.connectIfNeeded();

    yield {
      name: 'WebSocket connection',
      status: ((): Result<string> => {
        if (!status.ok) return status;
        return okResult('Connected');
      })(),
    };

    if (!this.connected) return;
    const svcStatus = await this.getServiceState();

    yield {
      name: 'Traced version',
      status: ((): Result<string> => {
        if (!svcStatus.ok) return svcStatus;
        return okResult(svcStatus.value.tracingServiceVersion ?? 'N/A');
      })(),
    };

    if (svcStatus === undefined) return;

    yield {
      name: 'Traced state',
      status: ((): Result<string> => {
        if (!svcStatus.ok) return svcStatus;
        const tss = svcStatus.value;
        return okResult(
          `#producers: ${tss.producers?.length ?? 'N/A'}, ` +
            `#datasources: ${tss.dataSources?.length ?? 'N/A'}, ` +
            `#sessions: ${tss.numSessionsStarted ?? 'N/A'}`,
        );
      })(),
    };
  }

  private async connectIfNeeded(): Promise<Result<TracingProtocol>> {
    return this.mgmtConsumer.getOrCreate(() => this.createConsumerIpcChannel());
  }

  disconnect(): void {
    this.mgmtConsumer.value?.close();
    this.mgmtConsumer.reset();
  }

  async getServiceState(): Promise<Result<protos.ITracingServiceState>> {
    const ipcStatus = await this.connectIfNeeded();
    if (!ipcStatus.ok) return ipcStatus;
    const consumerIpc = ipcStatus.value;
    const req = new protos.QueryServiceStateRequest({});
    const rpcCall = consumerIpc.invokeStreaming('QueryServiceState', req);
    const resp = await rpcCall.promise;
    if (!exists(resp.serviceState)) {
      return errResult('Failed to decode QueryServiceStateResponse');
    }
    return okResult(resp.serviceState);
  }

  async startTracing(
    traceConfig: protos.ITraceConfig,
  ): Promise<Result<ConsumerIpcTracingSession>> {
    const ipcStatus = await this.createConsumerIpcChannel();
    if (!ipcStatus.ok) return ipcStatus;
    const consumerIpc = ipcStatus.value;
    const session = new ConsumerIpcTracingSession(consumerIpc, traceConfig);
    return okResult(session);
  }

  private async createConsumerIpcChannel(): Promise<Result<TracingProtocol>> {
    const maybeSock = await AsyncWebsocket.connect(this.wsUrl);
    if (maybeSock == undefined) {
      return errResult(
        `Failed to connect ${this.wsUrl}. ${websocketInstructions()}`,
      );
    }
    const stream = new WebSocketStream(maybeSock.release());
    const consumerIpc = await TracingProtocol.create(stream);
    return okResult(consumerIpc);
  }
}
