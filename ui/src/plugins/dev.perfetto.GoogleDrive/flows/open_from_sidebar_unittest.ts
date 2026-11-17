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

import {errResult, okResult, type Result} from '../../../base/result';
import type {App} from '../../../public/app';
import {
  alertConnectFailed,
  alertDownloadFailed,
  alertPickerFailed,
} from '../dialogs';
import type {
  DriveError,
  DriveClient,
  PickerResult,
} from '../google/drive_client';
import type {AuthError, GoogleOAuthClient} from '../google/oauth_client';
import {openFileFromSidebar} from './open_from_sidebar';

// The dialogs are replaced wholesale: the flow only decides which one to show.
vi.mock('../dialogs', () => ({
  alertConnectFailed: vi.fn(),
  alertPickerFailed: vi.fn(),
  alertDownloadFailed: vi.fn(),
}));

const TOKEN = 'token';
const FILE = {
  id: 'file123',
  name: 'my.pftrace',
} as google.picker.DocumentObject;
const CONTENTS = new ArrayBuffer(8);

// What each step of the flow returns. Defaults to everything succeeding, so
// each test only states the step it wants to fail.
interface Outcomes {
  readonly auth?: Result<string, AuthError>;
  readonly picked?: PickerResult;
  readonly download?: Result<ArrayBuffer, DriveError>;
}

function setup(outcomes: Outcomes = {}) {
  const authenticate = vi.fn(async () => outcomes.auth ?? okResult(TOKEN));
  const pickFile = vi.fn(async () => outcomes.picked ?? okResult(FILE));
  const openFile = vi.fn(async () => outcomes.download ?? okResult(CONTENTS));
  const openTraceFromBuffer = vi.fn();

  const run = () =>
    openFileFromSidebar(
      {openTraceFromBuffer} as unknown as App,
      {authenticate} as unknown as GoogleOAuthClient,
      {pickFile, openFile} as unknown as DriveClient,
    );

  return {run, authenticate, pickFile, openFile, openTraceFromBuffer};
}

function expectNoAlerts() {
  expect(alertConnectFailed).not.toHaveBeenCalled();
  expect(alertPickerFailed).not.toHaveBeenCalled();
  expect(alertDownloadFailed).not.toHaveBeenCalled();
}

describe('openFileFromSidebar', () => {
  beforeEach(() => {
    vi.clearAllMocks();
  });

  test('opens the picked file as a non-shareable trace', async () => {
    const t = setup();
    await t.run();

    expect(t.pickFile).toHaveBeenCalledWith(TOKEN);
    expect(t.openFile).toHaveBeenCalledWith(TOKEN, FILE.id);
    expect(t.openTraceFromBuffer).toHaveBeenCalledWith({
      buffer: CONTENTS,
      title: 'my.pftrace',
      fileName: 'my.pftrace',
      shareable: false,
      // Records which Drive file the trace is, for "Share to GDrive".
      pluginArgs: {'dev.perfetto.GoogleDrive': {fileId: FILE.id}},
    });
    expectNoAlerts();
  });

  test('asks only for the drive.file scope', async () => {
    const t = setup();
    await t.run();

    expect(t.authenticate).toHaveBeenCalledWith([
      'https://www.googleapis.com/auth/drive.file',
    ]);
  });

  test('falls back to a placeholder name for a nameless file', async () => {
    const t = setup({
      picked: okResult({id: 'file123'} as google.picker.DocumentObject),
    });
    await t.run();

    expect(t.openTraceFromBuffer).toHaveBeenCalledWith(
      expect.objectContaining({title: 'Google Drive file'}),
    );
  });

  test('reports a failed connection and stops', async () => {
    const error: AuthError = {type: 'popup_blocked'};
    const t = setup({auth: errResult(error)});
    await t.run();

    expect(alertConnectFailed).toHaveBeenCalledWith(error);
    expect(t.pickFile).not.toHaveBeenCalled();
    expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
  });

  test('stops silently when the picker is cancelled', async () => {
    const t = setup({picked: errResult({type: 'cancelled'})});
    await t.run();

    expectNoAlerts();
    expect(t.openFile).not.toHaveBeenCalled();
    expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
  });

  test('reports a picker failure and stops', async () => {
    const error = {type: 'error', cause: 'boom'} as const;
    const t = setup({picked: errResult(error)});
    await t.run();

    expect(alertPickerFailed).toHaveBeenCalledWith(error);
    expect(t.openFile).not.toHaveBeenCalled();
    expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
  });

  test('reports a failed download and opens nothing', async () => {
    const error: DriveError = {type: 'access_denied'};
    const t = setup({download: errResult(error)});
    await t.run();

    expect(alertDownloadFailed).toHaveBeenCalledWith(error, 'my.pftrace');
    expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
  });
});
