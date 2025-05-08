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

import protobuf from 'protobufjs/minimal';
import protos from '../../../protos';

import {ByteStream} from '../interfaces/byte_stream';
import {ProtoRingBuffer} from '../../../trace_processor/proto_ring_buffer';
import {defer} from '../../../base/deferred';
import {exists} from '../../../base/utils';
import {assertExists, assertFalse, assertTrue} from '../../../base/logging';
import {PacketAssembler} from './packet_assembler';
import {ResizableArrayBuffer} from '../../../base/resizable_array_buffer';

/**
 * Implements the Consumer side of the Perfetto Tracing Protocol.
 * https://perfetto.dev/docs/design-docs/api-and-abi#socket-protocol
 *
 * The passed stream must be a byte stream to the traced consumer port,
 * e.g. obatained by connecting adb to the /dev/socket/traced_consumer.
 */
export class TracingProtocol {
  private rxBuf = new ProtoRingBuffer('FIXED_SIZE');

  private pendingInvokes = new Map<number, PendingInvoke>();

  // Wire protocol request ID. After each request it is increased. It is needed
  // to keep track of the type of request, and parse the response correctly.
  // We start from 2 because the static create() method takes the first one
  // for binding the service.
  private requestId = 2;

  onClose = () => {};

  // We have a separate factory method to await the initial service binding, so
  // we can return an object that is functional (methods can be invoked) and
  // avoid buffering.
  static async create(stream: ByteStream): Promise<TracingProtocol> {
    // Send the bindService request. This is a one-off request to connect to the
    // consumer port and list the RPC methods available.
    const requestId = 1;
    const txFrame = new protos.IPCFrame({
      requestId,
      msgBindService: new protos.IPCFrame.BindService({
        serviceName: 'ConsumerPort',
      }),
    });
    const repsponsePromise = defer<Uint8Array>();
    const rxFrameBuf = new ProtoRingBuffer('FIXED_SIZE');
    stream.onData = (data) => {
      rxFrameBuf.append(data);
      const rxFrame = rxFrameBuf.readMessage();
      rxFrame && repsponsePromise.resolve(rxFrame);
    };
    TracingProtocol.sendFrame(stream, txFrame);

    // Wait for the IPC reply. There is no state machine or queueing needed at
    // this point (not just yet) because this is 1 req -> 1 reply.
    const frameData = await repsponsePromise;
    const rxFrame = protos.IPCFrame.decode(frameData);
    assertTrue(rxFrame.msg === 'msgBindServiceReply');
    const replyMsg = assertExists(rxFrame.msgBindServiceReply);
    const boundMethods = new Map<string, number>();
    assertTrue(replyMsg.success === true);
    const serviceId = assertExists(replyMsg.serviceId);
    for (const m of assertExists(replyMsg.methods)) {
      boundMethods.set(assertExists(m.name), assertExists(m.id));
    }
    // Now that the details of the RPC methods are known, build and return the
    // TracingProtocol object, so the caller can finally make calls.
    return new TracingProtocol(stream, serviceId, boundMethods);
  }

  private constructor(
    private stream: ByteStream,
    private serviceId: number,
    private boundMethods: Map<string, number>,
  ) {
    stream.onData = this.onStreamData.bind(this);
    stream.onClose = () => this.close();
  }

  async invoke<T extends RpcMethodName>(
    methodName: T,
    req: RequestType<T>,
  ): Promise<ResponseType<T>> {
    const method = RPC_METHODS[methodName];
    const resultPromise = defer<ResponseType<T>>();
    const pendingInvoke: PendingInvoke = {
      methodName,
      failSilently: 'failSilently' in method && method.failSilently,
      onResponse: (data: Uint8Array | undefined, hasMore: boolean) => {
        assertFalse(hasMore); // Should have used invokeStreaming instead.
        const response = exists(data)
          ? method.respType.decode(data)
          : method.respType.create();
        resultPromise.resolve(response as ResponseType<T>);
      },
    };
    this.beginInvoke(methodName, req, pendingInvoke);
    return resultPromise;
  }

  invokeStreaming<T extends RpcStreamingMethodName>(
    methodName: T,
    req: RequestType<T>,
  ): StreamingResponseType<T> {
    const method = RPC_STREAMING_METHODS[methodName];
    const streamDecoder = method.respType.createStreamingDecoder();

    const pendingInvoke: PendingInvoke = {
      methodName,
      onResponse: (data: Uint8Array | undefined, hasMore: boolean) => {
        streamDecoder.decode(data, hasMore);
      },
    };
    this.beginInvoke(methodName, req, pendingInvoke);
    return streamDecoder as StreamingResponseType<T>;
  }

  // This call can arrive from two plaes:
  // 1. The user clicking on Stop/Cancel. In this case ConsumerIpcTracingSession
  //    calls this.consumerIpc.close().
  // 2. Stream disconnected is detected (e.g. the user pulls the cable). In this
  //    case we get here via stream.onClose = () => this.close().
  close() {
    if (this.stream.connected) {
      this.stream.close();
    }
    this.pendingInvokes.clear();
    this.onClose();
  }

  get connected() {
    return this.stream.connected;
  }

  [Symbol.dispose]() {
    this.close();
  }

  private beginInvoke<T extends RpcMethodName | RpcStreamingMethodName>(
    methodName: T,
    req: RequestType<T>,
    pendingInvoke: PendingInvoke,
  ): void {
    const methodId = this.boundMethods.get(methodName);
    if (methodId === undefined) {
      throw new Error(`RPC Error: method ${methodName} not supported`);
    }
    const requestId = this.requestId++;
    const argType =
      methodName in RPC_METHODS
        ? RPC_METHODS[methodName as RpcMethodName].argType
        : RPC_STREAMING_METHODS[methodName as RpcStreamingMethodName].argType;
    const argsProto: Uint8Array = argType.encode(req).finish();
    const frame = new protos.IPCFrame({
      requestId,
      msgInvokeMethod: new protos.IPCFrame.InvokeMethod({
        serviceId: this.serviceId,
        methodId: methodId,
        argsProto,
      }),
    });
    TracingProtocol.sendFrame(this.stream, frame);
    this.pendingInvokes.set(requestId, pendingInvoke);
  }

  private onStreamData(data: Uint8Array): void {
    this.rxBuf.append(data);
    for (;;) {
      const frameData = this.rxBuf.readMessage();
      if (frameData === undefined) break;
      this.parseFrame(frameData);
    }
  }

  private parseFrame(frameData: Uint8Array): void {
    // Get a copy of the ArrayBuffer to avoid the original being overriden.
    // See 170256902#comment21
    const frame = protos.IPCFrame.decode(frameData.slice());
    if (frame.msg === 'msgInvokeMethodReply') {
      const reply = assertExists(frame.msgInvokeMethodReply);
      const pendInvoke = assertExists(this.pendingInvokes.get(frame.requestId));
      // We process messages without a `replyProto` field (for instance
      // `FreeBuffers` does not have `replyProto`). However, we ignore messages
      // without a valid 'success' field.
      if (reply.success === false && !pendInvoke.failSilently) {
        throw new Error(`Tracing Protocol: ${pendInvoke.methodName} failed`);
      }
      pendInvoke.onResponse(
        reply.replyProto ?? undefined,
        Boolean(reply.hasMore),
      );
      if (!reply.hasMore) {
        this.pendingInvokes.delete(frame.requestId);
      }
    } else {
      throw new Error(`Tracing protocol: unrecognized frame ${frame.msg}`);
    }
  }

  private static sendFrame(
    stream: ByteStream,
    frame: protos.IPCFrame,
  ): Promise<void> {
    const writer = protobuf.Writer.create();
    writer.fixed32(0); // Reserve space for the 4 bytes header (frame len).
    const frameData = protos.IPCFrame.encode(frame, writer).finish().slice();
    const frameLen = frameData.length - 4;
    const dv = new DataView(frameData.buffer);
    dv.setUint32(0, frameLen, /* littleEndian */ true); // Write the header.
    return stream.write(frameData);
  }
}

export class PacketStream {
  static createStreamingDecoder(): PacketStream {
    return new PacketStream();
  }

  private traceBuf = new PacketAssembler();

  onTraceData: (packets: Uint8Array, hasMore: boolean) => void = () => {};

  decode(data: Uint8Array | undefined, hasMore: boolean) {
    if (data === undefined) {
      this.onTraceData(new Uint8Array(), hasMore);
      return;
    }

    // ReadBuffers returns 1+ slices. They can form 1 packet (usually),
    // >1 packet, or a fraction of a packet.
    const rdresp = protos.ReadBuffersResponse.decode(data);
    const packets: Uint8Array = this.traceBuf.pushSlices(rdresp);
    this.onTraceData(packets, hasMore);
  }
}

// QueryServiceStateResponse can be split in several chunks if the service state
// exceeds the 128KB ipc limit. This class simply merges them and exposes the
// merged result once hasMore = false.
class ServiceStateMerger {
  static createStreamingDecoder(): ServiceStateMerger {
    return new ServiceStateMerger();
  }

  private rxBuf = new ResizableArrayBuffer();
  readonly promise = defer<protos.QueryServiceStateResponse>();

  decode(data: Uint8Array | undefined, hasMore: boolean) {
    if (data !== undefined) {
      this.rxBuf.append(data);
    }

    if (!hasMore) {
      const msg = protos.QueryServiceStateResponse.decode(this.rxBuf.get());
      this.rxBuf.clear();
      this.promise.resolve(msg);
    }
  }
}

const RPC_METHODS = {
  EnableTracing: {
    argType: protos.EnableTracingRequest,
    respType: protos.EnableTracingResponse,
  },
  DisableTracing: {
    argType: protos.DisableTracingRequest,
    respType: protos.DisableTracingResponse,
  },
  Flush: {
    argType: protos.FlushRequest,
    respType: protos.FlushResponse,
    failSilently: true,
  },
  FreeBuffers: {
    argType: protos.FreeBuffersRequest,
    respType: protos.FreeBuffersResponse,
  },
  GetTraceStats: {
    argType: protos.GetTraceStatsRequest,
    respType: protos.GetTraceStatsResponse,
  },
};

const RPC_STREAMING_METHODS = {
  ReadBuffers: {
    argType: protos.ReadBuffersRequest,
    respType: PacketStream,
  },
  QueryServiceState: {
    argType: protos.QueryServiceStateRequest,
    respType: ServiceStateMerger,
  },
};

type RpcMethods = typeof RPC_METHODS;
type RpcStreamingMethods = typeof RPC_STREAMING_METHODS;

export type RpcMethodName = keyof RpcMethods & string;
export type RpcStreamingMethodName = keyof RpcStreamingMethods & string;
export type RpcAllMethodName = RpcMethodName | RpcStreamingMethodName;

type RequestType<T extends RpcAllMethodName> = InstanceType<
  (RpcMethods & RpcStreamingMethods)[T]['argType']
>;

type ResponseType<T extends RpcMethodName> = InstanceType<
  RpcMethods[T]['respType']
>;

type StreamingResponseType<T extends RpcStreamingMethodName> = InstanceType<
  RpcStreamingMethods[T]['respType']
>;

interface PendingInvoke {
  methodName: RpcAllMethodName; // This exists only to make debugging easier.
  onResponse: (data: Uint8Array | undefined, hasMore: boolean) => void;
  failSilently?: boolean;
}
