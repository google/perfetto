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

export enum ConversionJobStatus {
  InProgress = 'InProgress',
  NotRunning = 'NotRunning',
}

export type ConversionJobName =
  | 'convert_systrace'
  | 'convert_json'
  | 'open_in_legacy'
  | 'convert_pprof'
  | 'create_permalink';

export interface ConversionJobStatusUpdate {
  jobName: ConversionJobName;
  jobStatus: ConversionJobStatus;
}
