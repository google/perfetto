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

import {Evt} from '../../../base/events';
import {RecordingTarget} from './recording_target';

/**
 * The contract for the object returned by {@link RecordingTarget.startTracing}.
 */
export interface TracingSession {
  readonly state: TracingSessionState;
  readonly logs: ReadonlyArray<TracingSessionLogEntry>;
  readonly onSessionUpdate: Evt<void>;

  /** Stop tracing and get the data captured so far. */
  stop(): Promise<void>;

  /** Stop tracing and discard the data. */
  cancel(): Promise<void>;

  /* Returns the percentage of the trace buffer that is currently used */
  getBufferUsagePct(): Promise<number | undefined>;

  /** Returns the trace file captured once state === 'FINISHED'. */
  getTraceData(): Uint8Array | undefined;
}

export type TracingSessionState =
  | 'RECORDING'
  | 'STOPPING'
  | 'FINISHED'
  | 'ERRORED';

export interface TracingSessionLogEntry {
  readonly timestamp: Date;
  readonly message: string;
  readonly isError?: boolean;
}
