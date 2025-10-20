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

import {assertExists, assertFalse} from '../../../base/logging';
import {getOrCreate} from '../../../base/utils';
import protos from '../../../protos';

export const FTRACE_DS = 'linux.ftrace';
export type RecordMode = 'STOP_WHEN_FULL' | 'RING_BUFFER' | 'LONG_TRACE';
export type BufferMode = 'DISCARD' | 'RING_BUFFER';

export const DEFAULT_BUFFER_ID = 'default';

export class TraceConfigBuilder {
  readonly buffers = new Map<string, BufferConfig>();
  readonly dataSources = new Map<string, DataSource>();

  // The default values here don't matter, they exist only to make the TS
  // compiler happy. The actual defaults are defined by serialization_schema.ts.
  mode: RecordMode = 'STOP_WHEN_FULL';
  durationMs = 10_000;
  maxFileSizeMb = 0;
  fileWritePeriodMs = 0;
  compression = false;

  constructor() {
    this.buffers.set(DEFAULT_BUFFER_ID, {sizeKb: 64 * 1024});
  }

  get defaultBuffer(): BufferConfig {
    return assertExists(this.buffers.get(DEFAULT_BUFFER_ID));
  }

  // It has get-or-create semantics.
  addDataSource(name: string, targetBufId?: string): protos.IDataSourceConfig {
    return getOrCreate(this.dataSources, name + targetBufId, () => ({
      targetBufId,
      config: {name},
    })).config;
  }

  addBuffer(id: string, sizeKb: number, mode?: BufferMode) {
    assertFalse(this.buffers.has(id));
    this.buffers.set(id, {sizeKb, mode});
  }

  addFtraceEvents(...ftraceEvents: string[]) {
    const cfg = this.addDataSource('linux.ftrace');
    cfg.ftraceConfig ??= {};
    cfg.ftraceConfig.ftraceEvents ??= [];
    cfg.ftraceConfig.ftraceEvents.push(...ftraceEvents);
  }

  addAtraceApps(...apps: string[]) {
    const cfg = this.addDataSource('linux.ftrace');
    cfg.ftraceConfig ??= {};
    cfg.ftraceConfig.atraceApps ??= [];
    cfg.ftraceConfig.atraceApps.push(...apps);
  }

  addAtraceCategories(...cats: string[]) {
    const cfg = this.addDataSource('linux.ftrace');
    cfg.ftraceConfig ??= {};
    cfg.ftraceConfig.atraceCategories ??= [];
    cfg.ftraceConfig.atraceCategories.push(...cats);
  }

  addTrackEventEnabledCategories(...cats: string[]) {
    const cfg = this.addDataSource('track_event');
    cfg.trackEventConfig ??= {};
    cfg.trackEventConfig.enabledCategories ??= [];
    cfg.trackEventConfig.enabledCategories.push(...cats);
  }

  addTrackEventDisabledCategories(...cats: string[]) {
    const cfg = this.addDataSource('track_event');
    cfg.trackEventConfig ??= {};
    cfg.trackEventConfig.disabledCategories ??= [];
    cfg.trackEventConfig.disabledCategories.push(...cats);
  }

  toTraceConfig(): protos.TraceConfig {
    const traceCfg = new protos.TraceConfig();
    traceCfg.durationMs = this.durationMs;
    if (this.mode === 'LONG_TRACE') {
      traceCfg.writeIntoFile = true;
      traceCfg.fileWritePeriodMs = this.fileWritePeriodMs;
      traceCfg.maxFileSizeBytes = this.maxFileSizeMb * 1_000_000;
    }

    if (this.compression) {
      traceCfg.compressionType =
        protos.TraceConfig.CompressionType.COMPRESSION_TYPE_DEFLATE;
    }

    const orderedBufIds = [];
    for (const [id, buf] of this.buffers.entries()) {
      const fillPolicy =
        buf.mode === 'DISCARD' ||
        (buf.mode === undefined && this.mode === 'STOP_WHEN_FULL')
          ? protos.TraceConfig.BufferConfig.FillPolicy.DISCARD
          : protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER;
      traceCfg.buffers.push({sizeKb: buf.sizeKb, fillPolicy});
      orderedBufIds.push(id);
    }
    for (const ds of this.dataSources.values()) {
      let targetBuffer: number | undefined = undefined;
      if (ds.targetBufId !== undefined) {
        targetBuffer = orderedBufIds.indexOf(ds.targetBufId);
        if (targetBuffer < 0) {
          throw new Error(
            `DataSource ${ds.config.name} specified buffer id ` +
              `${ds.targetBufId} but it doesn't exist. ` +
              `Buffers: [${orderedBufIds.join(',')}]`,
          );
        }
      }
      traceCfg.dataSources.push({config: {...ds.config, targetBuffer}});
    }
    return traceCfg;
  }
}

export interface DataSource {
  config: protos.IDataSourceConfig;
  targetBufId?: string;
}

export interface BufferConfig {
  sizeKb: number;
  // If omitted infers from the config-wide mode.
  mode?: BufferMode;
}
