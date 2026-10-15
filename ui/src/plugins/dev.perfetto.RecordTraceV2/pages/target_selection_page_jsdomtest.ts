// Copyright (C) 2026 The Android Open Source Project
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

import m from 'mithril';
import {waitFor} from '@testing-library/dom';
import {afterEach, beforeEach, expect, test, vi} from 'vitest';
import {assertTrue, ensureExists} from '../../../base/assert';
import {EvtSource} from '../../../base/events';
import {errResult, okResult} from '../../../base/result';
import type {App} from '../../../public/app';
import type {PreflightCheckResult} from '../interfaces/connection_check';
import type {RecordingTarget} from '../interfaces/recording_target';
import type {RecordingTargetProvider} from '../interfaces/recording_target_provider';
import {RecordingManager} from '../recording_manager';
import {targetSelectionPage} from './target_selection_page';

vi.mock('../../../base/proto_utils_wasm', () => ({
  traceConfigToPb: vi.fn(),
  traceConfigToTxt: vi.fn(),
}));

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, []);
  container.remove();
});

test.each(['adbStatus=OFFLINE', 'traced is not running'])(
  'refresh devices repeats target checks after %s',
  async (error) => {
    let status: PreflightCheckResult = errResult(error);
    const checkTarget = vi.fn();
    const target: RecordingTarget = {
      id: 'device',
      name: 'Android device',
      platform: 'ANDROID',
      connected: false,
      async *runPreflightChecks() {
        checkTarget();
        yield {name: 'Target connection', status};
      },
      getServiceState: async () => okResult({}),
      disconnect: vi.fn(),
      startTracing: async () => errResult('Not used by this test'),
    };
    const provider: RecordingTargetProvider = {
      id: 'provider',
      name: 'Transport',
      icon: 'usb',
      description: '',
      supportedPlatforms: ['ANDROID'],
      onTargetsChanged: new EvtSource<void>(),
      async *runPreflightChecks() {
        yield {name: 'Transport connection', status: okResult('Connected')};
      },
      listTargets: async () => [target],
    };
    const app = {raf: {scheduleFullRedraw: vi.fn()}} as unknown as App;
    const recMgr = new RecordingManager(app);
    recMgr.registerProvider(provider);
    await recMgr.setProvider(provider);
    const page = targetSelectionPage(recMgr);
    assertTrue(page.kind === 'GLOBAL_PAGE');
    const render = () => m.render(container, page.render());
    render();
    await waitFor(() => {
      render();
      expect(container.textContent).toContain(error);
    });
    expect(checkTarget).toHaveBeenCalledTimes(1);

    status = okResult('Ready to record');
    const refresh = ensureExists(
      container.querySelector<HTMLButtonElement>('[title="Refresh devices"]'),
    );
    refresh.click();
    render();
    await waitFor(() => {
      render();
      expect(container.textContent).toContain('Ready to record');
    });
    expect(container.textContent).not.toContain(error);
    expect(checkTarget).toHaveBeenCalledTimes(2);
    expect(recMgr.currentTarget).toBe(target);
  },
);
