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

import {produce} from 'immer';
import {assertExists} from '../base/logging';
import {StateActions} from './actions';
import {createEmptyState} from './empty_state';
import {TraceUrlSource} from '../public/trace_info';

test('open trace', () => {
  const state = createEmptyState();
  const recordConfig = state.recordConfig;
  const after = produce(state, (draft) => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/bar',
    });
  });

  expect(after.engine).not.toBeUndefined();
  expect((after.engine!!.source as TraceUrlSource).url).toBe(
    'https://example.com/bar',
  );
  expect(after.recordConfig).toBe(recordConfig);
});

test('open second trace from file', () => {
  const once = produce(createEmptyState(), (draft) => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/bar',
    });
  });

  const thrice = produce(once, (draft) => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/foo',
    });
  });

  expect(thrice.engine).not.toBeUndefined();
  expect((thrice.engine!!.source as TraceUrlSource).url).toBe(
    'https://example.com/foo',
  );
});

test('setEngineReady with missing engine is ignored', () => {
  const state = createEmptyState();
  produce(state, (draft) => {
    StateActions.setEngineReady(draft, {
      engineId: '1',
      ready: true,
      mode: 'WASM',
    });
  });
});

test('setEngineReady', () => {
  const state = createEmptyState();
  const after = produce(state, (draft) => {
    StateActions.openTraceFromUrl(draft, {
      url: 'https://example.com/bar',
    });
    const latestEngineId = assertExists(draft.engine).id;
    StateActions.setEngineReady(draft, {
      engineId: latestEngineId,
      ready: true,
      mode: 'WASM',
    });
  });
  expect(after.engine!!.ready).toBe(true);
});
