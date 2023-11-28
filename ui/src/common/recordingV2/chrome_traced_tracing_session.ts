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

import {defer, Deferred} from '../../base/deferred';
import {assertExists, assertTrue} from '../../base/logging';
import {binaryDecode, binaryEncode} from '../../base/string_utils';
import {
  ChromeExtensionMessage,
  isChromeExtensionError,
  isChromeExtensionStatus,
  isGetCategoriesResponse,
} from '../../controller/chrome_proxy_record_controller';
import {
  isDisableTracingResponse,
  isEnableTracingResponse,
  isFreeBuffersResponse,
  isGetTraceStatsResponse,
  isReadBuffersResponse,
} from '../../controller/consumer_port_types';
import {
  EnableTracingRequest,
  IBufferStats,
  ISlice,
  TraceConfig,
} from '../../protos';

import {RecordingError} from './recording_error_handling';
import {
  TracingSession,
  TracingSessionListener,
} from './recording_interfaces_v2';
import {
  BUFFER_USAGE_INCORRECT_FORMAT,
  BUFFER_USAGE_NOT_ACCESSIBLE,
  EXTENSION_ID,
  MALFORMED_EXTENSION_MESSAGE,
} from './recording_utils';

// This class implements the protocol described in
// https://perfetto.dev/docs/design-docs/api-and-abi#tracing-protocol-abi
// However, with the Chrome extension we communicate using JSON messages.
export class ChromeTracedTracingSession implements TracingSession {
  // Needed for ReadBufferResponse: all the trace packets are split into
  // several slices. |partialPacket| is the buffer for them. Once we receive a
  // slice with the flag |lastSliceForPacket|, a new packet is created.
  private partialPacket: ISlice[] = [];

  // For concurrent calls to 'GetCategories', we return the same value.
  private pendingGetCategoriesMessage?: Deferred<string[]>;

  private pendingStatsMessages = new Array<Deferred<IBufferStats[]>>();

  // Port through which we communicate with the extension.
  private chromePort: chrome.runtime.Port;
  // True when Perfetto is connected via the port to the tracing session.
  private isPortConnected: boolean;

  constructor(private tracingSessionListener: TracingSessionListener) {
    this.chromePort = chrome.runtime.connect(EXTENSION_ID);
    this.isPortConnected = true;
  }

  start(config: TraceConfig): void {
    if (!this.isPortConnected) return;
    const duration = config.durationMs;
    this.tracingSessionListener.onStatus(`Recording in progress${
        duration ? ' for ' + duration.toString() + ' ms' : ''}...`);

    const enableTracingRequest = new EnableTracingRequest();
    enableTracingRequest.traceConfig = config;
    const enableTracingRequestProto = binaryEncode(
        EnableTracingRequest.encode(enableTracingRequest).finish());
    this.chromePort.postMessage(
        {method: 'EnableTracing', requestData: enableTracingRequestProto});
  }

  // The 'cancel' method will end the tracing session and will NOT return the
  // trace. Therefore, we do not need to keep the connection open.
  cancel(): void {
    if (!this.isPortConnected) return;
    this.terminateConnection();
  }

  // The 'stop' method will end the tracing session and cause the trace to be
  // returned via a callback. We maintain the connection to the target so we can
  // extract the trace.
  // See 'DisableTracing' in:
  // https://perfetto.dev/docs/design-docs/life-of-a-tracing-session
  stop(): void {
    if (!this.isPortConnected) return;
    this.chromePort.postMessage({method: 'DisableTracing'});
  }

  getCategories(): Promise<string[]> {
    if (!this.isPortConnected) {
      throw new RecordingError(
          'Attempting to get categories from a ' +
          'disconnected tracing session.');
    }
    if (this.pendingGetCategoriesMessage) {
      return this.pendingGetCategoriesMessage;
    }

    this.chromePort.postMessage({method: 'GetCategories'});
    return this.pendingGetCategoriesMessage = defer<string[]>();
  }

  async getTraceBufferUsage(): Promise<number> {
    if (!this.isPortConnected) return 0;
    const bufferStats = await this.getBufferStats();
    let percentageUsed = -1;
    for (const buffer of bufferStats) {
      const used = assertExists(buffer.bytesWritten);
      const total = assertExists(buffer.bufferSize);
      if (total >= 0) {
        percentageUsed = Math.max(percentageUsed, used / total);
      }
    }

    if (percentageUsed === -1) {
      throw new RecordingError(BUFFER_USAGE_INCORRECT_FORMAT);
    }
    return percentageUsed;
  }

  initConnection(): void {
    this.chromePort.onMessage.addListener((message: ChromeExtensionMessage) => {
      this.handleExtensionMessage(message);
    });
  }

  private getBufferStats(): Promise<IBufferStats[]> {
    this.chromePort.postMessage({method: 'GetTraceStats'});

    const statsMessage = defer<IBufferStats[]>();
    this.pendingStatsMessages.push(statsMessage);
    return statsMessage;
  }

  private terminateConnection(): void {
    this.chromePort.postMessage({method: 'FreeBuffers'});
    this.clearState();
  }

  private clearState() {
    this.chromePort.disconnect();
    this.isPortConnected = false;
    for (const statsMessage of this.pendingStatsMessages) {
      statsMessage.reject(new RecordingError(BUFFER_USAGE_NOT_ACCESSIBLE));
    }
    this.pendingStatsMessages = [];
    this.pendingGetCategoriesMessage = undefined;
  }

  private handleExtensionMessage(message: ChromeExtensionMessage) {
    if (isChromeExtensionError(message)) {
      this.terminateConnection();
      this.tracingSessionListener.onError(message.error);
    } else if (isChromeExtensionStatus(message)) {
      this.tracingSessionListener.onStatus(message.status);
    } else if (isReadBuffersResponse(message)) {
      if (!message.slices) {
        return;
      }
      for (const messageSlice of message.slices) {
        // The extension sends the binary data as a string.
        // see http://shortn/_oPmO2GT6Vb
        if (typeof messageSlice.data !== 'string') {
          throw new RecordingError(MALFORMED_EXTENSION_MESSAGE);
        }
        const decodedSlice = {
          data: binaryDecode(messageSlice.data),
        };
        this.partialPacket.push(decodedSlice);
        if (messageSlice.lastSliceForPacket) {
          let bufferSize = 0;
          for (const slice of this.partialPacket) {
            bufferSize += slice.data!.length;
          }

          const completeTrace = new Uint8Array(bufferSize);
          let written = 0;
          for (const slice of this.partialPacket) {
            const data = slice.data!;
            completeTrace.set(data, written);
            written += data.length;
          }
          // The trace already comes encoded as a proto.
          this.tracingSessionListener.onTraceData(completeTrace);
          this.terminateConnection();
        }
      }
    } else if (isGetCategoriesResponse(message)) {
      assertExists(this.pendingGetCategoriesMessage)
          .resolve(message.categories);
      this.pendingGetCategoriesMessage = undefined;
    } else if (isEnableTracingResponse(message)) {
      // Once the service notifies us that a tracing session is enabled,
      // we can start streaming the response using 'ReadBuffers'.
      this.chromePort.postMessage({method: 'ReadBuffers'});
    } else if (isGetTraceStatsResponse(message)) {
      const maybePendingStatsMessage = this.pendingStatsMessages.shift();
      if (maybePendingStatsMessage) {
        maybePendingStatsMessage.resolve(
            message?.traceStats?.bufferStats || []);
      }
    } else if (isFreeBuffersResponse(message)) {
      // No action required. If we successfully read a whole trace,
      // we close the connection. Alternatively, if the tracing finishes
      // with an exception or if the user cancels it, we also close the
      // connection.
    } else {
      assertTrue(isDisableTracingResponse(message));
      // No action required. Same reasoning as for FreeBuffers.
    }
  }
}
