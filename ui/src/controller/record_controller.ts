// Copyright (C) 2018 The Android Open Source Project
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

import {TraceConfig} from '../common/protos';
import {
  IProcessStatsConfig,
  ISysStatsConfig,
  ITraceConfig,
  MeminfoCounters,
  StatCounters,
  VmstatCounters
} from '../common/protos';
import {RecordConfig} from '../common/state';

import {Controller} from './controller';
import {App} from './globals';

export function uint8ArrayToBase64(buffer: Uint8Array): string {
  return btoa(String.fromCharCode.apply(null, buffer));
}

export function encodeConfig(config: RecordConfig): Uint8Array {
  const sizeKb = config.bufferSizeMb * 1024;
  const durationMs = config.durationSeconds * 1000;

  const dataSources = [];
  if (config.ftrace) {
    const drainPeriodMs =
        config.ftraceDrainPeriodMs ? config.ftraceDrainPeriodMs : null;
    const bufferSizeKb =
        config.ftraceBufferSizeKb ? config.ftraceBufferSizeKb : null;
    dataSources.push({
      config: {
        name: 'linux.ftrace',
        targetBuffer: 0,
        ftraceConfig: {
          ftraceEvents: config.ftraceEvents,
          atraceApps: config.atraceApps,
          atraceCategories: config.atraceCategories,
          drainPeriodMs,
          bufferSizeKb,
        },
      },
    });
  }

  if (config.processMetadata) {
    const processStatsConfig: IProcessStatsConfig = {
      scanAllProcessesOnStart: config.scanAllProcessesOnStart
    };

    if (config.procStatusPeriodMs) {
      processStatsConfig.procStatsPollMs = config.procStatusPeriodMs;
    }

    dataSources.push({
      config: {
        name: 'linux.process_stats',
        processStatsConfig,
        targetBuffer: 0,
      },
    });
  }

  if (config.sysStats) {
    const sysStatsConfig: ISysStatsConfig = {};

    if (config.meminfoPeriodMs) {
      sysStatsConfig.meminfoPeriodMs = config.meminfoPeriodMs;
      sysStatsConfig.meminfoCounters = config.meminfoCounters.map(name => {
        // tslint:disable-next-line no-any
        return MeminfoCounters[name as any as number] as any as number;
      });
    }
    if (config.vmstatPeriodMs) {
      sysStatsConfig.vmstatPeriodMs = config.vmstatPeriodMs;
      sysStatsConfig.vmstatCounters = config.vmstatCounters.map(name => {
        // tslint:disable-next-line no-any
        return VmstatCounters[name as any as number] as any as number;
      });
    }
    if (config.statPeriodMs) {
      sysStatsConfig.statPeriodMs = config.statPeriodMs;
      sysStatsConfig.statCounters = config.statCounters.map(name => {
        // tslint:disable-next-line no-any
        return StatCounters[name as any as number] as any as number;
      });
    }

    dataSources.push({
      config: {
        name: 'linux.sys_stats',
        sysStatsConfig,
      },
    });
  }

  const proto: ITraceConfig = {
    durationMs,
    buffers: [
      {
        sizeKb,
      },
    ],
    dataSources,
  };

  if (config.writeIntoFile) {
    proto.writeIntoFile = true;
    if (config.fileWritePeriodMs) {
      proto.fileWritePeriodMs = config.fileWritePeriodMs;
    }
  }

  const buffer = TraceConfig.encode(proto).finish();
  return buffer;
}

export function toPbtxt(configBuffer: Uint8Array): string {
  const msg = TraceConfig.decode(configBuffer);
  const json = msg.toJSON();
  function snakeCase(s: string): string {
    return s.replace(/[A-Z]/g, c => '_' + c.toLowerCase());
  }
  // With the ahead of time compiled protos we can't seem to tell which
  // fields are enums.
  function looksLikeEnum(value: string): boolean {
    return value.startsWith('MEMINFO_') || value.startsWith('VMSTAT_') ||
        value.startsWith('STAT_');
  }
  function* message(msg: {}, indent: number): IterableIterator<string> {
    for (const [key, value] of Object.entries(msg)) {
      const isRepeated = Array.isArray(value);
      const isNested = typeof value === 'object' && !isRepeated;
      for (const entry of (isRepeated ? value as Array<{}>: [value])) {
        yield ' '.repeat(indent) + `${snakeCase(key)}${isNested ? '' : ':'} `;
        if (typeof entry === 'string') {
          yield looksLikeEnum(entry) ? entry : `"${entry}"`;
        } else if (typeof entry === 'number') {
          yield entry.toString();
        } else if (typeof entry === 'boolean') {
          yield entry.toString();
        } else {
          yield '{\n';
          yield* message(entry, indent + 4);
          yield ' '.repeat(indent) + '}';
        }
        yield '\n';
      }
    }
  }
  return [...message(json, 0)].join('');
}

export class RecordController extends Controller<'main'> {
  private app: App;
  private config: RecordConfig|null = null;

  constructor(args: {app: App}) {
    super('main');
    this.app = args.app;
  }

  run() {
    if (this.app.state.recordConfig === this.config) return;
    this.config = this.app.state.recordConfig;
    const configProto = encodeConfig(this.config);
    const configProtoText = toPbtxt(configProto);
    const commandline = `
      echo '${uint8ArrayToBase64(configProto)}' |
      base64 --decode |
      adb shell "perfetto -c - -o /data/misc/perfetto-traces/trace" &&
      adb pull /data/misc/perfetto-traces/trace /tmp/trace
    `;
    // TODO(hjd): This should not be TrackData after we unify the stores.
    this.app.publish('TrackData', {
      id: 'config',
      data: {
        commandline,
        pbtxt: configProtoText,
      }
    });
  }
}
