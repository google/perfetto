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
import type {AdbDevice} from './adb_device';
import {TracingProtocol} from '../tracing_protocol/tracing_protocol';
import {errResult, okResult, type Result} from '../../../base/result';
import {exists} from '../../../base/utils';
import {ConsumerIpcTracingSession} from '../tracing_protocol/consumer_ipc_tracing_session';

let tracedSocket = '/dev/socket/traced_consumer';
export function setTracedSocket(socket: string) {
  tracedSocket = socket;
}

export function createAdbTracingSession(
  adbDevice: AdbDevice,
  traceConfig: protos.ITraceConfig,
): Promise<Result<ConsumerIpcTracingSession>> {
  return ConsumerIpcTracingSession.create({
    ipcFactory: () => openAdbConsumerIpc(adbDevice),
    traceConfig,
  });
}

async function openAdbConsumerIpc(
  adbDevice: AdbDevice,
): Promise<Result<TracingProtocol>> {
  const streamStatus = await adbDevice.createStream(
    getTracedConsumerSocketAddressForAdb(),
  );
  if (!streamStatus.ok) return streamStatus;
  return okResult(await TracingProtocol.create(streamStatus.value));
}

export async function getAdbTracingServiceState(
  adbDevice: AdbDevice,
): Promise<Result<protos.ITracingServiceState>> {
  const status = await adbDevice.createStream(
    getTracedConsumerSocketAddressForAdb(),
  );
  if (!status.ok) {
    return errResult(`Failed to connect to ${tracedSocket}: ${status.error}`);
  }
  const stream = status.value;
  using consumerPort = await TracingProtocol.create(stream);
  const req = new protos.QueryServiceStateRequest({});
  const rpcCall = consumerPort.invokeStreaming('QueryServiceState', req);
  const resp = await rpcCall.promise;
  if (!exists(resp.serviceState)) {
    return errResult('Failed to decode QueryServiceStateResponse');
  }
  return okResult(resp.serviceState);
}

// Return the fully formed ADB socket address according to the settings
// The address is of the form <type>:<address>
function getTracedConsumerSocketAddressForAdb() {
  if (tracedSocket.startsWith('@')) {
    return `localabstract:${tracedSocket.slice(1)}`;
  }
  return `localfilesystem:${tracedSocket}`;
}
