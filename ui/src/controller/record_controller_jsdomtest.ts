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

import {dingus} from 'dingusjs';

import {TraceConfig} from '../common/protos';
import {createEmptyRecordConfig, RecordConfig} from '../common/state';

import {App} from './globals';
import {
  genConfigProto,
  RecordController,
  toPbtxt,
  uint8ArrayToBase64
} from './record_controller';

test('uint8ArrayToBase64', () => {
  const bytes = [...'Hello, world'].map(c => c.charCodeAt(0));
  const buffer = new Uint8Array(bytes);
  expect(uint8ArrayToBase64(buffer)).toEqual('SGVsbG8sIHdvcmxk');
});

test('encodeConfig', () => {
  const config = createEmptyRecordConfig();
  config.durationSeconds = 10;
  const result = TraceConfig.decode(genConfigProto(config));
  expect(result.durationMs).toBe(10000);
});

test('toPbtxt', () => {
  const config = {
    durationMs: 1000,
    buffers: [
      {
        sizeKb: 42,
      },
    ],
    dataSources: [{
      config: {
        name: 'linux.ftrace',
        targetBuffer: 1,
        ftraceConfig: {
          ftraceEvents: ['sched_switch', 'print'],
        },
      },
    }],
    producers: [
      {
        producerName: 'perfetto.traced_probes',
      },
    ],
  };

  const text = toPbtxt(TraceConfig.encode(config).finish());

  expect(text).toEqual(`buffers: {
    size_kb: 42
}
data_sources: {
    config {
        name: "linux.ftrace"
        target_buffer: 1
        ftrace_config {
            ftrace_events: "sched_switch"
            ftrace_events: "print"
        }
    }
}
duration_ms: 1000
producers: {
    producer_name: "perfetto.traced_probes"
}
`);
});

test('RecordController', () => {
  const app = dingus<App>('globals');
  (app.state.recordConfig as RecordConfig) = createEmptyRecordConfig();
  const controller = new RecordController({app});
  controller.run();
  controller.run();
  controller.run();
  // tslint:disable-next-line no-any
  const calls = app.calls.filter((call: any) => call[0] === 'publish()');
  expect(calls.length).toBe(1);
  // TODO(hjd): Fix up dingus to have a more sensible API.
  expect(calls[0][1][0]).toEqual('TrackData');
});
