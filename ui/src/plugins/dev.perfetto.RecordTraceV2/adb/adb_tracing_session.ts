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
import {AdbDevice} from './adb_device';
import {TracingProtocol} from '../tracing_protocol/tracing_protocol';
import {errResult, okResult, Result} from '../../../base/result';
import {exists} from '../../../base/utils';
import {ConsumerIpcTracingSession} from '../tracing_protocol/consumer_ipc_tracing_session';

let tracedSocket = '/dev/socket/traced_consumer';
export function setTracedSocket(socket: string) {
  tracedSocket = socket;
}

export async function createAdbTracingSession(
  adbDevice: AdbDevice,
  traceConfig: protos.ITraceConfig,
): Promise<Result<ConsumerIpcTracingSession>> {
  const streamStatus = await adbDevice.createStream(
    getTracedConsumerSocketAddressForAdb(),
  );
  if (!streamStatus.ok) return streamStatus;
  const stream = streamStatus.value;
  const consumerIpc = await TracingProtocol.create(stream);
  const session = new ConsumerIpcTracingSession(consumerIpc, traceConfig);
  return okResult(session);
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

// Clones an in-progress tracing session identified by `uniqueSessionName` and
// returns the cloned trace as a single concatenated byte array.
//
// Opens a fresh consumer connection rather than reusing an existing one: the
// CloneSession RPC attaches the consumer to the cloned session, so sharing a
// connection would detach any in-progress recording on the original session.
// The connection is closed before this function returns.
export async function cloneAdbTracingSession(
  adbDevice: AdbDevice,
  uniqueSessionName: string,
): Promise<Result<Uint8Array>> {
  // Create a new connection for the clone operation
  const streamStatus = await adbDevice.createStream(
    getTracedConsumerSocketAddressForAdb(),
  );
  if (!streamStatus.ok) return streamStatus;
  const stream = streamStatus.value;
  const consumerIpc = await TracingProtocol.create(stream);

  try {
    // Clone the session by name
    const cloneResp = await consumerIpc.invoke(
      'CloneSession',
      new protos.CloneSessionRequest({uniqueSessionName}),
    );

    if (!cloneResp.success) {
      consumerIpc.close();
      return errResult(cloneResp.error || 'CloneSession failed');
    }

    // Read the cloned trace data
    const traceData = await readClonedData(consumerIpc);
    consumerIpc.close();
    return okResult(traceData);
  } catch (e) {
    consumerIpc.close();
    return errResult(`CloneSession error: ${e}`);
  }
}

function readClonedData(consumerIpc: TracingProtocol): Promise<Uint8Array> {
  return new Promise((resolve) => {
    const chunks: Uint8Array[] = [];
    const stream = consumerIpc.invokeStreaming(
      'ReadBuffers',
      new protos.ReadBuffersRequest({}),
    );
    stream.onTraceData = (data: Uint8Array, hasMore: boolean) => {
      chunks.push(data);
      if (!hasMore) {
        // Concatenate all chunks
        const totalLen = chunks.reduce((sum, c) => sum + c.length, 0);
        const result = new Uint8Array(totalLen);
        let offset = 0;
        for (const chunk of chunks) {
          result.set(chunk, offset);
          offset += chunk.length;
        }
        resolve(result);
      }
    };
  });
}

// Return the fully formed ADB socket address according to the settings
// The address is of the form <type>:<address>
function getTracedConsumerSocketAddressForAdb() {
  if (tracedSocket.startsWith('@')) {
    return `localabstract:${tracedSocket.slice(1)}`;
  }
  return `localfilesystem:${tracedSocket}`;
}
