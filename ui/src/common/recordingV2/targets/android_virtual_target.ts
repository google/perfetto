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

import {RecordingError} from '../recording_error_handling';
import {
  RecordingTargetV2,
  TargetInfo,
  TracingSession,
  TracingSessionListener,
} from '../recording_interfaces_v2';

export class AndroidVirtualTarget implements RecordingTargetV2 {
  constructor(
    private name: string,
    private androidApiLevel: number,
  ) {}

  canConnectWithoutContention(): Promise<boolean> {
    return Promise.resolve(true);
  }

  canCreateTracingSession(): boolean {
    return false;
  }

  createTracingSession(_: TracingSessionListener): Promise<TracingSession> {
    throw new RecordingError(
      'Can not create tracing session for a virtual target',
    );
  }

  disconnect(_?: string): Promise<void> {
    throw new RecordingError('Can not disconnect from a virtual target');
  }

  fetchTargetInfo(_: TracingSessionListener): Promise<void> {
    return Promise.resolve();
  }

  getInfo(): TargetInfo {
    return {
      name: this.name,
      androidApiLevel: this.androidApiLevel,
      targetType: 'ANDROID',
      dataSources: [],
    };
  }
}
