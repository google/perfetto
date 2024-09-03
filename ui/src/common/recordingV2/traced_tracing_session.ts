// Copyright (C) 2022 The Android Open Source Project
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

import protobuf from 'protobufjs/minimal';
import {defer, Deferred} from '../../base/deferred';
import {assertExists, assertFalse, assertTrue} from '../../base/logging';
import {
  DisableTracingRequest,
  DisableTracingResponse,
  EnableTracingRequest,
  EnableTracingResponse,
  FreeBuffersRequest,
  FreeBuffersResponse,
  GetTraceStatsRequest,
  GetTraceStatsResponse,
  IBufferStats,
  IMethodInfo,
  IPCFrame,
  ISlice,
  QueryServiceStateRequest,
  QueryServiceStateResponse,
  ReadBuffersRequest,
  ReadBuffersResponse,
  TraceConfig,
} from '../../protos';
import {RecordingError} from './recording_error_handling';
import {
  ByteStream,
  DataSource,
  TracingSession,
  TracingSessionListener,
} from './recording_interfaces_v2';
import {
  BUFFER_USAGE_INCORRECT_FORMAT,
  BUFFER_USAGE_NOT_ACCESSIBLE,
  PARSING_UNABLE_TO_DECODE_METHOD,
  PARSING_UNKNWON_REQUEST_ID,
  PARSING_UNRECOGNIZED_MESSAGE,
  PARSING_UNRECOGNIZED_PORT,
  RECORDING_IN_PROGRESS,
} from './recording_utils';
import {exists} from '../../base/utils';

// See wire_protocol.proto for more details.
const WIRE_PROTOCOL_HEADER_SIZE = 4;
// See basic_types.h (kIPCBufferSize) for more details.
const MAX_IPC_BUFFER_SIZE = 128 * 1024;

const PROTO_LEN_DELIMITED_WIRE_TYPE = 2;
const TRACE_PACKET_PROTO_ID = 1;
const TRACE_PACKET_PROTO_TAG =
  (TRACE_PACKET_PROTO_ID << 3) | PROTO_LEN_DELIMITED_WIRE_TYPE;

function parseMessageSize(buffer: Uint8Array) {
  const dv = new DataView(buffer.buffer, buffer.byteOffset, buffer.length);
  return dv.getUint32(0, true);
}

// This class implements the protocol described in
// https://perfetto.dev/docs/design-docs/api-and-abi#tracing-protocol-abi
export class TracedTracingSession implements TracingSession {
  // Buffers received wire protocol data.
  private incomingBuffer = new Uint8Array(MAX_IPC_BUFFER_SIZE);
  private bufferedPartLength = 0;
  private currentFrameLength?: number;

  private availableMethods: IMethodInfo[] = [];
  private serviceId = -1;

  private resolveBindingPromise!: Deferred<void>;
  private requestMethods = new Map<number, string>();

  // Needed for ReadBufferResponse: all the trace packets are split into
  // several slices. |partialPacket| is the buffer for them. Once we receive a
  // slice with the flag |lastSliceForPacket|, a new packet is created.
  private partialPacket: ISlice[] = [];
  // Accumulates trace packets into a proto trace file..
  private traceProtoWriter = protobuf.Writer.create();

  // Accumulates DataSource objects from QueryServiceStateResponse,
  // which can have >1 replies for each query
  // go/codesearch/android/external/perfetto/protos/
  // perfetto/ipc/consumer_port.proto;l=243-246
  private pendingDataSources: DataSource[] = [];

  // For concurrent calls to 'QueryServiceState', we return the same value.
  private pendingQssMessage?: Deferred<DataSource[]>;

  // Wire protocol request ID. After each request it is increased. It is needed
  // to keep track of the type of request, and parse the response correctly.
  private requestId = 1;

  private pendingStatsMessages = new Array<Deferred<IBufferStats[]>>();

  // The bytestream is obtained when creating a connection with a target.
  // For instance, the AdbStream is obtained from a connection with an Adb
  // device.
  constructor(
    private byteStream: ByteStream,
    private tracingSessionListener: TracingSessionListener,
  ) {
    this.byteStream.addOnStreamDataCallback((data) =>
      this.handleReceivedData(data),
    );
    this.byteStream.addOnStreamCloseCallback(() => this.clearState());
  }

  queryServiceState(): Promise<DataSource[]> {
    if (this.pendingQssMessage) {
      return this.pendingQssMessage;
    }

    const requestProto = QueryServiceStateRequest.encode(
      new QueryServiceStateRequest(),
    ).finish();
    this.rpcInvoke('QueryServiceState', requestProto);

    return (this.pendingQssMessage = defer<DataSource[]>());
  }

  start(config: TraceConfig): void {
    const duration = config.durationMs;
    this.tracingSessionListener.onStatus(
      `${RECORDING_IN_PROGRESS}${
        duration ? ' for ' + duration.toString() + ' ms' : ''
      }...`,
    );

    const enableTracingRequest = new EnableTracingRequest();
    enableTracingRequest.traceConfig = config;
    const enableTracingRequestProto =
      EnableTracingRequest.encode(enableTracingRequest).finish();
    this.rpcInvoke('EnableTracing', enableTracingRequestProto);
  }

  cancel(): void {
    this.terminateConnection();
  }

  stop(): void {
    const requestProto = DisableTracingRequest.encode(
      new DisableTracingRequest(),
    ).finish();
    this.rpcInvoke('DisableTracing', requestProto);
  }

  async getTraceBufferUsage(): Promise<number> {
    if (!this.byteStream.isConnected()) {
      // TODO(octaviant): make this more in line with the other trace buffer
      //  error cases.
      return 0;
    }
    const bufferStats = await this.getBufferStats();
    let percentageUsed = -1;
    for (const buffer of bufferStats) {
      if (
        !Number.isFinite(buffer.bytesWritten) ||
        !Number.isFinite(buffer.bufferSize)
      ) {
        continue;
      }
      const used = assertExists(buffer.bytesWritten);
      const total = assertExists(buffer.bufferSize);
      if (total >= 0) {
        percentageUsed = Math.max(percentageUsed, used / total);
      }
    }

    if (percentageUsed === -1) {
      return Promise.reject(new RecordingError(BUFFER_USAGE_INCORRECT_FORMAT));
    }
    return percentageUsed;
  }

  initConnection(): Promise<void> {
    // bind IPC methods
    const requestId = this.requestId++;
    const frame = new IPCFrame({
      requestId,
      msgBindService: new IPCFrame.BindService({serviceName: 'ConsumerPort'}),
    });
    this.writeFrame(frame);

    // We shouldn't bind multiple times to the service in the same tracing
    // session.
    // eslint-disable-next-line @typescript-eslint/strict-boolean-expressions
    assertFalse(!!this.resolveBindingPromise);
    this.resolveBindingPromise = defer<void>();
    return this.resolveBindingPromise;
  }

  private getBufferStats(): Promise<IBufferStats[]> {
    const getTraceStatsRequestProto = GetTraceStatsRequest.encode(
      new GetTraceStatsRequest(),
    ).finish();
    try {
      this.rpcInvoke('GetTraceStats', getTraceStatsRequestProto);
    } catch (e) {
      // GetTraceStats was introduced only on Android 10.
      this.raiseError(e);
    }

    const statsMessage = defer<IBufferStats[]>();
    this.pendingStatsMessages.push(statsMessage);
    return statsMessage;
  }

  private terminateConnection(): void {
    this.clearState();
    const requestProto = FreeBuffersRequest.encode(
      new FreeBuffersRequest(),
    ).finish();
    this.rpcInvoke('FreeBuffers', requestProto);
    this.byteStream.close();
  }

  private clearState() {
    for (const statsMessage of this.pendingStatsMessages) {
      statsMessage.reject(new RecordingError(BUFFER_USAGE_NOT_ACCESSIBLE));
    }
    this.pendingStatsMessages = [];
    this.pendingDataSources = [];
    this.pendingQssMessage = undefined;
  }

  private rpcInvoke(methodName: string, argsProto: Uint8Array): void {
    if (!this.byteStream.isConnected()) {
      return;
    }
    const method = this.availableMethods.find((m) => m.name === methodName);
    if (!exists(method) || !exists(method.id)) {
      throw new RecordingError(
        `Method ${methodName} not supported by the target`,
      );
    }
    const requestId = this.requestId++;
    const frame = new IPCFrame({
      requestId,
      msgInvokeMethod: new IPCFrame.InvokeMethod({
        serviceId: this.serviceId,
        methodId: method.id,
        argsProto,
      }),
    });
    this.requestMethods.set(requestId, methodName);
    this.writeFrame(frame);
  }

  private writeFrame(frame: IPCFrame): void {
    const frameProto: Uint8Array = IPCFrame.encode(frame).finish();
    const frameLen = frameProto.length;
    const buf = new Uint8Array(WIRE_PROTOCOL_HEADER_SIZE + frameLen);
    const dv = new DataView(buf.buffer);
    dv.setUint32(0, frameProto.length, /* littleEndian */ true);
    for (let i = 0; i < frameLen; i++) {
      dv.setUint8(WIRE_PROTOCOL_HEADER_SIZE + i, frameProto[i]);
    }
    this.byteStream.write(buf);
  }

  private handleReceivedData(rawData: Uint8Array): void {
    // we parse the length of the next frame if it's available
    if (
      this.currentFrameLength === undefined &&
      this.canCompleteLengthHeader(rawData)
    ) {
      const remainingFrameBytes =
        WIRE_PROTOCOL_HEADER_SIZE - this.bufferedPartLength;
      this.appendToIncomingBuffer(rawData.subarray(0, remainingFrameBytes));
      rawData = rawData.subarray(remainingFrameBytes);

      this.currentFrameLength = parseMessageSize(this.incomingBuffer);
      this.bufferedPartLength = 0;
    }

    // Parse all complete frames.
    while (
      this.currentFrameLength !== undefined &&
      this.bufferedPartLength + rawData.length >= this.currentFrameLength
    ) {
      // Read the remaining part of this message.
      const bytesToCompleteMessage =
        this.currentFrameLength - this.bufferedPartLength;
      this.appendToIncomingBuffer(rawData.subarray(0, bytesToCompleteMessage));
      this.parseFrame(this.incomingBuffer.subarray(0, this.currentFrameLength));
      this.bufferedPartLength = 0;
      // Remove the data just parsed.
      rawData = rawData.subarray(bytesToCompleteMessage);

      if (!this.canCompleteLengthHeader(rawData)) {
        this.currentFrameLength = undefined;
        break;
      }
      this.currentFrameLength = parseMessageSize(rawData);
      rawData = rawData.subarray(WIRE_PROTOCOL_HEADER_SIZE);
    }

    // Buffer the remaining data (part of the next message).
    this.appendToIncomingBuffer(rawData);
  }

  private canCompleteLengthHeader(newData: Uint8Array): boolean {
    return newData.length + this.bufferedPartLength > WIRE_PROTOCOL_HEADER_SIZE;
  }

  private appendToIncomingBuffer(array: Uint8Array): void {
    this.incomingBuffer.set(array, this.bufferedPartLength);
    this.bufferedPartLength += array.length;
  }

  private parseFrame(frameBuffer: Uint8Array): void {
    // Get a copy of the ArrayBuffer to avoid the original being overriden.
    // See 170256902#comment21
    const frame = IPCFrame.decode(frameBuffer.slice());
    if (frame.msg === 'msgBindServiceReply') {
      const msgBindServiceReply = frame.msgBindServiceReply;
      if (
        exists(msgBindServiceReply) &&
        exists(msgBindServiceReply.methods) &&
        exists(msgBindServiceReply.serviceId)
      ) {
        assertTrue(msgBindServiceReply.success === true);
        this.availableMethods = msgBindServiceReply.methods;
        this.serviceId = msgBindServiceReply.serviceId;
        this.resolveBindingPromise.resolve();
      }
    } else if (frame.msg === 'msgInvokeMethodReply') {
      const msgInvokeMethodReply = frame.msgInvokeMethodReply;
      // We process messages without a `replyProto` field (for instance
      // `FreeBuffers` does not have `replyProto`). However, we ignore messages
      // without a valid 'success' field.
      if (msgInvokeMethodReply?.success !== true) {
        return;
      }

      const method = this.requestMethods.get(frame.requestId);
      if (!method) {
        this.raiseError(`${PARSING_UNKNWON_REQUEST_ID}: ${frame.requestId}`);
        return;
      }
      const decoder = decoders.get(method);
      if (decoder === undefined) {
        this.raiseError(`${PARSING_UNABLE_TO_DECODE_METHOD}: ${method}`);
        return;
      }
      const data = {...decoder(msgInvokeMethodReply.replyProto)};

      if (method === 'ReadBuffers') {
        for (const slice of data.slices ?? []) {
          this.partialPacket.push(slice);
          if (slice.lastSliceForPacket === true) {
            let bufferSize = 0;
            for (const slice of this.partialPacket) {
              bufferSize += slice.data!.length;
            }
            const tracePacket = new Uint8Array(bufferSize);
            let written = 0;
            for (const slice of this.partialPacket) {
              const data = slice.data!;
              tracePacket.set(data, written);
              written += data.length;
            }
            this.traceProtoWriter.uint32(TRACE_PACKET_PROTO_TAG);
            this.traceProtoWriter.bytes(tracePacket);
            this.partialPacket = [];
          }
        }
        if (msgInvokeMethodReply.hasMore === false) {
          this.tracingSessionListener.onTraceData(
            this.traceProtoWriter.finish(),
          );
          this.terminateConnection();
        }
      } else if (method === 'EnableTracing') {
        const readBuffersRequestProto = ReadBuffersRequest.encode(
          new ReadBuffersRequest(),
        ).finish();
        this.rpcInvoke('ReadBuffers', readBuffersRequestProto);
      } else if (method === 'GetTraceStats') {
        const maybePendingStatsMessage = this.pendingStatsMessages.shift();
        if (maybePendingStatsMessage) {
          maybePendingStatsMessage.resolve(data?.traceStats?.bufferStats ?? []);
        }
      } else if (method === 'FreeBuffers') {
        // No action required. If we successfully read a whole trace,
        // we close the connection. Alternatively, if the tracing finishes
        // with an exception or if the user cancels it, we also close the
        // connection.
      } else if (method === 'DisableTracing') {
        // No action required. Same reasoning as for FreeBuffers.
      } else if (method === 'QueryServiceState') {
        const dataSources =
          (data as QueryServiceStateResponse)?.serviceState?.dataSources || [];
        for (const dataSource of dataSources) {
          const name = dataSource?.dsDescriptor?.name;
          if (name) {
            this.pendingDataSources.push({
              name,
              descriptor: dataSource.dsDescriptor,
            });
          }
        }
        if (msgInvokeMethodReply.hasMore === false) {
          assertExists(this.pendingQssMessage).resolve(this.pendingDataSources);
          this.pendingDataSources = [];
          this.pendingQssMessage = undefined;
        }
      } else {
        this.raiseError(`${PARSING_UNRECOGNIZED_PORT}: ${method}`);
      }
    } else {
      this.raiseError(`${PARSING_UNRECOGNIZED_MESSAGE}: ${frame.msg}`);
    }
  }

  private raiseError(message: string): void {
    this.terminateConnection();
    this.tracingSessionListener.onError(message);
  }
}

const decoders = new Map<string, Function>()
  .set('EnableTracing', EnableTracingResponse.decode)
  .set('FreeBuffers', FreeBuffersResponse.decode)
  .set('ReadBuffers', ReadBuffersResponse.decode)
  .set('DisableTracing', DisableTracingResponse.decode)
  .set('GetTraceStats', GetTraceStatsResponse.decode)
  .set('QueryServiceState', QueryServiceStateResponse.decode);
