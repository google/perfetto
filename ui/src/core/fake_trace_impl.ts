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

import z from 'zod';
import {Time} from '../base/time';
import {EngineBase} from '../trace_processor/engine';
import {AppImpl, createApp} from './app_impl';
import {InMemoryStorage} from './in_memory_storage';
import {SettingsManagerImpl} from './settings_manager';
import {TraceImpl} from './trace_impl';
import {TraceInfoImpl} from './trace_info_impl';
import {DurationPrecision, TimestampFormat} from '../public/timeline';
import {commandInvocationArraySchema} from './command_manager';
import {ErrorDetails} from '../base/logging';

export interface FakeTraceImplArgs {
  // If true suppresses exceptions when trying to issue a query. This is to
  // catch bugs where we are trying to query an empty instance. However some
  // unittests need to do so. Default: false.
  allowQueries?: boolean;
}

export function initializeAppImplForTesting(): AppImpl {
  const settingsManager = new SettingsManagerImpl(new InMemoryStorage());
  return createApp({
    initialRouteArgs: {},
    settingsManager,
    timestampFormatSetting: settingsManager.register({
      id: 'timestampFormat',
      name: 'Timestamp Format',
      description: '',
      defaultValue: TimestampFormat.Timecode,
      schema: z.nativeEnum(TimestampFormat),
    }),
    durationPrecisionSetting: settingsManager.register({
      id: 'durationPrecision',
      name: 'Duration Precision',
      description: '',
      defaultValue: DurationPrecision.Full,
      schema: z.nativeEnum(DurationPrecision),
    }),
    timezoneOverrideSetting: settingsManager.register({
      id: 'timezoneOverride',
      name: 'Timezone Override',
      description: 'What timezone to use for displaying timestamps.',
      schema: z.enum(['dummy']),
      defaultValue: 'dummy',
    }),
    analyticsSetting: settingsManager.register({
      id: 'analyticsEnable',
      name: 'Enable UI Telemetry',
      description: '',
      schema: z.boolean(),
      defaultValue: true,
    }),
    startupCommandsSetting: settingsManager.register({
      id: 'startupCommands',
      name: 'Startup Commands',
      description: '',
      schema: commandInvocationArraySchema,
      defaultValue: [],
    }),
    maybeShowErrorDialog: (error: ErrorDetails) => {
      console.error(error);
    },
  });
}

// For testing purposes only.
export function createFakeTraceImpl(args: FakeTraceImplArgs = {}) {
  const app = initializeAppImplForTesting();
  const fakeTraceInfo: TraceInfoImpl = {
    source: {type: 'URL', url: ''},
    traceTitle: '',
    traceUrl: '',
    start: Time.fromSeconds(0),
    end: Time.fromSeconds(10),
    unixOffset: Time.ZERO,
    tzOffMin: 0,
    cpus: [],
    importErrors: 0,
    traceType: 'proto',
    hasFtrace: false,
    uuid: '',
    cached: false,
    downloadable: false,
  };
  app.closeCurrentTrace();
  const trace = TraceImpl.createInstanceForCore(
    app,
    new FakeEngine(args.allowQueries ?? false),
    fakeTraceInfo,
  );
  app.setActiveTrace(trace);
  return trace;
}

class FakeEngine extends EngineBase {
  readonly mode = 'WASM';
  id: string = 'TestEngine';

  constructor(private allowQueries: boolean) {
    super();
  }

  rpcSendRequestBytes(_data: Uint8Array) {
    if (!this.allowQueries) {
      throw new Error(
        'FakeEngine.query() should never be reached. ' +
          'If this is a unittest, try adding {allowQueries: true} to the ' +
          'createFakeTraceImpl() call.',
      );
    }
  }

  [Symbol.dispose]() {}
}
