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
import {Result} from '../../../base/result';
import {PreflightCheck, WithPreflightChecks} from './connection_check';
import {TargetPlatformId} from './target_platform';
import {TracingSession} from './tracing_session';

/**
 * The interface that models a device that can be used for recording a trace.
 * This is the contract that RecordingTargetProvider(s) must implement in order
 * to support recording. The UI bits don't care about the specific
 * implementation and only use this class.
 * Conceptually a RecordingTarget maps to a connection to the Consumer socket
 * to the tracing service.
 */
export interface RecordingTarget extends WithPreflightChecks {
  readonly id: string;
  readonly platform: TargetPlatformId;
  readonly name: string;
  readonly connected: boolean;

  // If true, the output file is gzip-compressed as a whole (!= than setting
  // deflate in the trace config). The chrome devtools protocol does this.
  readonly emitsCompressedtrace?: boolean;

  // Returns a list of debugging check to diagnose target connection failures.
  runPreflightChecks(): AsyncGenerator<PreflightCheck>;

  getServiceState(): Promise<Result<protos.ITracingServiceState>>;

  disconnect(): void;

  startTracing(
    traceConfig: protos.ITraceConfig,
  ): Promise<Result<TracingSession>>;
}
