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

import {z} from 'zod';

const recordModes = ['STOP_WHEN_FULL', 'RING_BUFFER', 'LONG_TRACE'] as const;
export const RECORD_CONFIG_SCHEMA = z
  .object({
    mode: z.enum(recordModes).default('STOP_WHEN_FULL'),
    durationMs: z.number().default(10000.0),
    maxFileSizeMb: z.number().default(100),
    fileWritePeriodMs: z.number().default(2500),
    bufferSizeMb: z.number().default(64.0),

    cpuSched: z.boolean().default(false),
    cpuFreq: z.boolean().default(false),
    cpuFreqPollMs: z.number().default(1000),
    cpuSyscall: z.boolean().default(false),

    gpuFreq: z.boolean().default(false),
    gpuMemTotal: z.boolean().default(false),
    gpuWorkPeriod: z.boolean().default(false),

    ftrace: z.boolean().default(false),
    atrace: z.boolean().default(false),
    ftraceEvents: z.array(z.string()).default([]),
    ftraceExtraEvents: z.string().default(''),
    atraceCats: z.array(z.string()).default([]),
    allAtraceApps: z.boolean().default(true),
    atraceApps: z.string().default(''),
    ftraceBufferSizeKb: z.number().default(0),
    ftraceDrainPeriodMs: z.number().default(0),
    androidLogs: z.boolean().default(false),
    androidLogBuffers: z.array(z.string()).default([]),
    androidFrameTimeline: z.boolean().default(false),
    androidGameInterventionList: z.boolean().default(false),
    androidNetworkTracing: z.boolean().default(false),
    androidNetworkTracingPollMs: z.number().default(250),

    cpuCoarse: z.boolean().default(false),
    cpuCoarsePollMs: z.number().default(1000),

    batteryDrain: z.boolean().default(false),
    batteryDrainPollMs: z.number().default(1000),

    boardSensors: z.boolean().default(false),

    memHiFreq: z.boolean().default(false),
    meminfo: z.boolean().default(false),
    meminfoPeriodMs: z.number().default(1000),
    meminfoCounters: z.array(z.string()).default([]),

    vmstat: z.boolean().default(false),
    vmstatPeriodMs: z.number().default(1000),
    vmstatCounters: z.array(z.string()).default([]),

    heapProfiling: z.boolean().default(false),
    hpSamplingIntervalBytes: z.number().default(4096),
    hpProcesses: z.string().default(''),
    hpContinuousDumpsPhase: z.number().default(0),
    hpContinuousDumpsInterval: z.number().default(0),
    hpSharedMemoryBuffer: z.number().default(8 * 1048576),
    hpBlockClient: z.boolean().default(true),
    hpAllHeaps: z.boolean().default(false),

    javaHeapDump: z.boolean().default(false),
    jpProcesses: z.string().default(''),
    jpContinuousDumpsPhase: z.number().default(0),
    jpContinuousDumpsInterval: z.number().default(0),

    memLmk: z.boolean().default(false),
    procStats: z.boolean().default(false),
    procStatsPeriodMs: z.number().default(1000),

    chromeCategoriesSelected: z.array(z.string()).default([]),
    chromeHighOverheadCategoriesSelected: z.array(z.string()).default([]),
    chromePrivacyFiltering: z.boolean().default(false),

    chromeLogs: z.boolean().default(false),
    taskScheduling: z.boolean().default(false),
    ipcFlows: z.boolean().default(false),
    jsExecution: z.boolean().default(false),
    webContentRendering: z.boolean().default(false),
    uiRendering: z.boolean().default(false),
    inputEvents: z.boolean().default(false),
    navigationAndLoading: z.boolean().default(false),
    audio: z.boolean().default(false),
    video: z.boolean().default(false),

    etwCSwitch: z.boolean().default(false),
    etwThreadState: z.boolean().default(false),

    symbolizeKsyms: z.boolean().default(false),

    // Enabling stack sampling
    tracePerf: z.boolean().default(false),
    timebaseFrequency: z.number().default(100),
    targetCmdLine: z.array(z.string()).default([]),

    linuxDeviceRpm: z.boolean().default(false),
  })
  // .default({}) ensures that we can always default-construct a config and
  // spots accidental missing .default(...)
  .default({});

export const NAMED_RECORD_CONFIG_SCHEMA = z.object({
  title: z.string(),
  key: z.string(),
  config: RECORD_CONFIG_SCHEMA,
});
export type NamedRecordConfig = z.infer<typeof NAMED_RECORD_CONFIG_SCHEMA>;
export type RecordConfig = z.infer<typeof RECORD_CONFIG_SCHEMA>;

export function createEmptyRecordConfig(): RecordConfig {
  return RECORD_CONFIG_SCHEMA.parse({});
}
