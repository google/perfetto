// Copyright (C) 2021 The Android Open Source Project
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

import {ConversionJobStatusUpdate} from '../common/conversion_jobs';
import {raf} from '../core/raf_scheduler';
import {HttpRpcState} from '../trace_processor/http_rpc_engine';
import {globals} from './globals';

export function publishTrackData(args: {id: string; data: {}}) {
  globals.setTrackData(args.id, args.data);
  raf.scheduleRedraw();
}

export function publishHttpRpcState(httpRpcState: HttpRpcState) {
  globals.httpRpcState = httpRpcState;
  raf.scheduleFullRedraw();
}

export function publishConversionJobStatusUpdate(
  job: ConversionJobStatusUpdate,
) {
  globals.setConversionJobStatus(job.jobName, job.jobStatus);
  globals.publishRedraw();
}

export function publishBufferUsage(args: {percentage: number}) {
  globals.setBufferUsage(args.percentage);
  globals.publishRedraw();
}

export function publishRecordingLog(args: {logs: string}) {
  globals.setRecordingLog(args.logs);
  globals.publishRedraw();
}

export function publishShowPanningHint() {
  globals.showPanningHint = true;
  globals.publishRedraw();
}

export function publishPermalinkHash(hash: string | undefined): void {
  globals.permalinkHash = hash;
  globals.publishRedraw();
}
