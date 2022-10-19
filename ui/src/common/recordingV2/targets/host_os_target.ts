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

import {HostOsByteStream} from '../host_os_byte_stream';
import {RecordingError} from '../recording_error_handling';
import {
  DataSource,
  HostOsTargetInfo,
  OnDisconnectCallback,
  OnTargetChangeCallback,
  RecordingTargetV2,
  TracingSession,
  TracingSessionListener,
} from '../recording_interfaces_v2';
import {
  isLinux,
  isMacOs,
  WEBSOCKET_CLOSED_ABNORMALLY_CODE,
} from '../recording_utils';
import {TracedTracingSession} from '../traced_tracing_session';

export class HostOsTarget implements RecordingTargetV2 {
  private readonly targetType: 'LINUX'|'MACOS';
  private readonly name: string;
  private websocket: WebSocket;
  private streams = new Set<HostOsByteStream>();
  private dataSources?: DataSource[];
  private onDisconnect: OnDisconnectCallback = (_) => {};

  constructor(
      websocketUrl: string,
      private maybeClearTarget: (target: HostOsTarget) => void,
      private onTargetChange: OnTargetChangeCallback) {
    if (isMacOs(navigator.userAgent)) {
      this.name = 'MacOS';
      this.targetType = 'MACOS';
    } else if (isLinux(navigator.userAgent)) {
      this.name = 'Linux';
      this.targetType = 'LINUX';
    } else {
      throw new RecordingError(
          'Host OS target created on an unsupported operating system.');
    }

    this.websocket = new WebSocket(websocketUrl);
    this.websocket.onclose = this.onClose.bind(this);
    // 'onError' gets called when the websocketURL where the UI tries to connect
    // is disallowed by the Content Security Policy. In this case, we disconnect
    // the target.
    this.websocket.onerror = this.disconnect.bind(this);
  }

  getInfo(): HostOsTargetInfo {
    return {
      targetType: this.targetType,
      name: this.name,
      dataSources: this.dataSources || [],
    };
  }

  canCreateTracingSession(): boolean {
    return true;
  }

  async createTracingSession(tracingSessionListener: TracingSessionListener):
      Promise<TracingSession> {
    this.onDisconnect = tracingSessionListener.onDisconnect;

    const osStream = await HostOsByteStream.create(this.getUrl());
    this.streams.add(osStream);
    const tracingSession =
        new TracedTracingSession(osStream, tracingSessionListener);
    await tracingSession.initConnection();

    if (!this.dataSources) {
      this.dataSources = await tracingSession.queryServiceState();
      this.onTargetChange();
    }
    return tracingSession;
  }

  // Starts a tracing session in order to fetch data sources from the
  // device. Then, it cancels the session.
  async fetchTargetInfo(tracingSessionListener: TracingSessionListener):
      Promise<void> {
    const tracingSession =
        await this.createTracingSession(tracingSessionListener);
    tracingSession.cancel();
  }

  async disconnect(): Promise<void> {
    if (this.websocket.readyState === this.websocket.OPEN) {
      this.websocket.close();
      // We remove the 'onclose' callback so the 'disconnect' method doesn't get
      // executed twice.
      this.websocket.onclose = null;
    }
    for (const stream of this.streams) {
      stream.close();
    }
    // We remove the existing target from the factory if present.
    this.maybeClearTarget(this);
    // We run the onDisconnect callback in case this target is used for tracing.
    this.onDisconnect();
  }

  // We can connect to the Host OS without taking the connection away from
  // another process.
  async canConnectWithoutContention(): Promise<boolean> {
    return true;
  }

  getUrl() {
    return this.websocket.url;
  }

  private onClose(ev: CloseEvent): void {
    if (ev.code === WEBSOCKET_CLOSED_ABNORMALLY_CODE) {
      console.info(
          `It's safe to ignore the 'WebSocket connection to ${
              this.getUrl()} error above, if present. It occurs when ` +
          'checking the connection to the local Websocket server.');
    }
    this.disconnect();
  }
}
