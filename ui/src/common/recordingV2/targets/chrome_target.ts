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

import {ChromeTracedTracingSession} from '../chrome_traced_tracing_session';
import {EXTENSION_ID} from '../chrome_utils';
import {
  ChromeTargetInfo,
  OnTargetChangeCallback,
  RecordingTargetV2,
  TracingSession,
  TracingSessionListener,
} from '../recording_interfaces_v2';

export class ChromeTarget implements RecordingTargetV2 {
  onTargetChange?: OnTargetChangeCallback;
  private chromeCategories?: string[];
  // We only check the connection once at the beginning to:
  // a) Avoid creating a 'Port' object every time 'getInfo' is called.
  // b) When a new Port is created, the extension starts communicating with it
  // and leaves aside the old Port objects, so creating a new Port would break
  // any ongoing tracing session.
  private isExtensionInstalled: boolean;

  constructor(private name: string, private targetType: 'CHROME'|'CHROME_OS') {
    const testPort = chrome.runtime.connect(EXTENSION_ID);
    this.isExtensionInstalled = !!testPort;
    testPort.disconnect();
  }

  getInfo(): ChromeTargetInfo {
    return {
      targetType: this.targetType,
      name: this.name,
      isExtensionInstalled: this.isExtensionInstalled,
      dataSources:
          [{name: 'chromeCategories', descriptor: this.chromeCategories}],
    };
  }

  async createTracingSession(tracingSessionListener: TracingSessionListener):
      Promise<TracingSession> {
    const tracingSession =
        new ChromeTracedTracingSession(tracingSessionListener);
    tracingSession.initConnection();

    if (!this.chromeCategories) {
      // Fetch chrome categories from the extension.
      this.chromeCategories = await tracingSession.getCategories();
      if (this.onTargetChange) {
        this.onTargetChange();
      }
    }

    return tracingSession;
  }

  // Starts a tracing session in order to fetch chrome categories from the
  // device. Then, it cancels the session.
  async fetchTargetInfo(tracingSessionListener: TracingSessionListener):
      Promise<void> {
    const tracingSession =
        await this.createTracingSession(tracingSessionListener);
    tracingSession.cancel();
  }

  disconnect(_disconnectMessage?: string): Promise<void> {
    return Promise.resolve(undefined);
  }

  // We can connect to the Chrome target without taking the connection away
  // from another process.
  async canConnectWithoutContention(): Promise<boolean> {
    return true;
  }
}
