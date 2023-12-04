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

import {
  oneOf,
  num,
  bool,
  arrayOf,
  str,
  requiredStr,
  record,
  runValidator,
  ValidatedType,
} from '../base/validators';

const recordModes = ['STOP_WHEN_FULL', 'RING_BUFFER', 'LONG_TRACE'] as const;
export const recordConfigValidator = record({
  mode: oneOf(recordModes, 'STOP_WHEN_FULL'),
  durationMs: num(10000.0),
  maxFileSizeMb: num(100),
  fileWritePeriodMs: num(2500),
  bufferSizeMb: num(64.0),

  cpuSched: bool(),
  cpuFreq: bool(),
  cpuFreqPollMs: num(1000),
  cpuSyscall: bool(),

  gpuFreq: bool(),
  gpuMemTotal: bool(),

  ftrace: bool(),
  atrace: bool(),
  ftraceEvents: arrayOf(str()),
  ftraceExtraEvents: str(),
  atraceCats: arrayOf(str()),
  allAtraceApps: bool(true),
  atraceApps: str(),
  ftraceBufferSizeKb: num(0),
  ftraceDrainPeriodMs: num(0),
  androidLogs: bool(),
  androidLogBuffers: arrayOf(str()),
  androidFrameTimeline: bool(),
  androidGameInterventionList: bool(),
  androidNetworkTracing: bool(),
  androidNetworkTracingPollMs: num(250),

  cpuCoarse: bool(),
  cpuCoarsePollMs: num(1000),

  batteryDrain: bool(),
  batteryDrainPollMs: num(1000),

  boardSensors: bool(),

  memHiFreq: bool(),
  meminfo: bool(),
  meminfoPeriodMs: num(1000),
  meminfoCounters: arrayOf(str()),

  vmstat: bool(),
  vmstatPeriodMs: num(1000),
  vmstatCounters: arrayOf(str()),

  heapProfiling: bool(),
  hpSamplingIntervalBytes: num(4096),
  hpProcesses: str(),
  hpContinuousDumpsPhase: num(),
  hpContinuousDumpsInterval: num(),
  hpSharedMemoryBuffer: num(8 * 1048576),
  hpBlockClient: bool(true),
  hpAllHeaps: bool(),

  javaHeapDump: bool(),
  jpProcesses: str(),
  jpContinuousDumpsPhase: num(),
  jpContinuousDumpsInterval: num(),

  memLmk: bool(),
  procStats: bool(),
  procStatsPeriodMs: num(1000),

  chromeCategoriesSelected: arrayOf(str()),
  chromeHighOverheadCategoriesSelected: arrayOf(str()),
  chromePrivacyFiltering: bool(),

  chromeLogs: bool(),
  taskScheduling: bool(),
  ipcFlows: bool(),
  jsExecution: bool(),
  webContentRendering: bool(),
  uiRendering: bool(),
  inputEvents: bool(),
  navigationAndLoading: bool(),

  symbolizeKsyms: bool(),

  // Enabling stack sampling
  tracePerf: bool(),
  timebaseFrequency: num(100),
  targetCmdLine: arrayOf(str()),
});
export const namedRecordConfigValidator = record(
    {title: requiredStr, key: requiredStr, config: recordConfigValidator});
export type NamedRecordConfig =
    ValidatedType<typeof namedRecordConfigValidator>;
export type RecordConfig = ValidatedType<typeof recordConfigValidator>;

export function createEmptyRecordConfig(): RecordConfig {
  return runValidator(recordConfigValidator, {}).result;
}
