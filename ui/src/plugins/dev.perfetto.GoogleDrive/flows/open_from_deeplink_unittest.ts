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
import type {Setting} from '../../../public/settings';
import {promptModal} from '../../../widgets/modal';
import {
  alertConnectFailed,
  alertDownloadFailed,
  alertPickerFailed,
} from '../dialogs';
import type {
  DriveClient,
  DriveError,
  GoogleDriveFileMeta,
  PickerResult,
} from '../google/drive_client';
import type {AuthError, GoogleOAuthClient} from '../google/oauth_client';
import {openFileFromDeeplink} from './open_from_deeplink';

// The alerts are replaced wholesale: the flow only decides which one to show.
vi.mock('../dialogs', () => ({
  alertConnectFailed: vi.fn(),
  alertPickerFailed: vi.fn(),
  alertDownloadFailed: vi.fn(),
}));

// The flow's two prompts are private to it, so they are answered one level
// down, through promptModal(). They are told apart by their modal key.
vi.mock('../../../widgets/modal', () => ({promptModal: vi.fn()}));

const CONNECT_PROMPT = 'GoogleDriveConnect';
const AUTHORIZE_PROMPT = 'GoogleDriveAuthNeeded';

const TOKEN = 'token';
const FILE_ID = 'file123';
const RESOURCE_KEY = 'rk-1';
const STATE = JSON.stringify({
  ids: [FILE_ID],
  action: 'open',
  resourceKeys: {[FILE_ID]: RESOURCE_KEY},
});
const CONTENTS = new ArrayBuffer(8);
const PICKED_FILE = {
  id: FILE_ID,
  name: 'picked.pftrace',
} as google.picker.DocumentObject;

const POPUP_BLOCKED = errResult<AuthError>({type: 'popup_blocked'});
const ACCESS_DENIED = errResult<DriveError>({type: 'access_denied'});

// What each step of the flow returns. Defaults to everything succeeding, so
// each test only states the steps it wants to go differently. Steps the flow
// can repeat take a list, answered in order; the last answer is repeated.
interface Outcomes {
  readonly auth?: ReadonlyArray<Result<string, AuthError>>;
  readonly meta?: Result<GoogleDriveFileMeta, DriveError>;
  readonly downloads?: ReadonlyArray<Result<ArrayBuffer, DriveError>>;
  readonly picked?: PickerResult;
  // Which button the user presses on each prompt. A prompt with no answer is
  // dismissed.
  readonly prompts?: Readonly<Record<string, string>>;
  // The user's "don't show this again" setting for the authorize prompt.
  readonly hideAuthDialog?: boolean;
}

// Returns a function that hands out `answers` in order, repeating the last.
function inOrder<T>(answers: ReadonlyArray<T>, fallback: T): () => T {
  let next = 0;
  return () => {
    const answer = answers[Math.min(next, answers.length - 1)] ?? fallback;
    next++;
    return answer;
  };
}

function setup(outcomes: Outcomes = {}) {
  const nextAuth = inOrder(outcomes.auth ?? [], okResult(TOKEN));
  const nextDownload = inOrder(outcomes.downloads ?? [], okResult(CONTENTS));

  const authenticate = vi.fn(async () => nextAuth());
  const getMeta = vi.fn(
    async () => outcomes.meta ?? okResult({name: 'meta.pftrace'}),
  );
  const openFile = vi.fn(async () => nextDownload());
  const requestFileAccess = vi.fn(
    async () => outcomes.picked ?? okResult(PICKED_FILE),
  );
  const openTraceFromBuffer = vi.fn();

  vi.mocked(promptModal).mockImplementation(
    async (args) => outcomes.prompts?.[args.key ?? ''] as never,
  );

  const run = (state = STATE) =>
    openFileFromDeeplink(
      {openTraceFromBuffer} as unknown as App,
      {authenticate} as unknown as GoogleOAuthClient,
      {getMeta, openFile, requestFileAccess} as unknown as DriveClient,
      state,
      {get: () => outcomes.hideAuthDialog ?? false} as Setting<boolean>,
    );

  return {
    run,
    authenticate,
    getMeta,
    openFile,
    requestFileAccess,
    openTraceFromBuffer,
  };
}

// The keys of the prompts shown to the user, in order.
function promptsShown(): Array<string | undefined> {
  return vi.mocked(promptModal).mock.calls.map(([args]) => args.key);
}

function expectNoAlerts() {
  expect(alertConnectFailed).not.toHaveBeenCalled();
  expect(alertPickerFailed).not.toHaveBeenCalled();
  expect(alertDownloadFailed).not.toHaveBeenCalled();
}

describe('openFileFromDeeplink', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    // The flow warns when the file's name cannot be looked up.
    vi.spyOn(console, 'warn').mockImplementation(() => {});
    history.replaceState(null, '', '/?state=abc&other=1');
  });

  afterEach(() => {
    vi.restoreAllMocks();
    history.replaceState(null, '', '/');
  });

  describe('when the file can be read straight away', () => {
    test('opens it as a non-shareable trace, with no prompts', async () => {
      const t = setup();
      await t.run();

      expect(t.openTraceFromBuffer).toHaveBeenCalledWith(
        expect.objectContaining({buffer: CONTENTS, shareable: false}),
      );
      expect(promptsShown()).toEqual([]);
      expect(t.requestFileAccess).not.toHaveBeenCalled();
      expectNoAlerts();
    });

    test('downloads the file named by the state, with its resource key', async () => {
      const t = setup();
      await t.run();

      expect(t.openFile).toHaveBeenCalledWith(TOKEN, FILE_ID, RESOURCE_KEY);
    });

    test("names the trace after the file's name in Drive", async () => {
      const t = setup({meta: okResult({name: 'from-drive.pftrace'})});
      await t.run();

      expect(t.openTraceFromBuffer).toHaveBeenCalledWith(
        expect.objectContaining({
          title: 'from-drive.pftrace',
          fileName: 'from-drive.pftrace',
        }),
      );
    });

    test('still opens the trace if its name cannot be looked up', async () => {
      const t = setup({meta: errResult({type: 'error'})});
      await t.run();

      expect(t.openTraceFromBuffer).toHaveBeenCalledWith(
        expect.objectContaining({
          buffer: CONTENTS,
          title: 'Google Drive file',
        }),
      );
      expectNoAlerts();
    });
  });

  describe('an invalid state parameter', () => {
    test.each([
      ['not JSON', 'not-json'],
      ['no file ID', JSON.stringify({ids: []})],
      ['an unsafe file ID', JSON.stringify({ids: ['../secret']})],
    ])('is ignored silently: %s', async (_name, state) => {
      const t = setup();
      await t.run(state);

      expect(t.authenticate).not.toHaveBeenCalled();
      expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
      expect(promptsShown()).toEqual([]);
      expectNoAlerts();
    });
  });

  describe('connecting', () => {
    test('asks the user to click, and retries, if the popup is blocked', async () => {
      const t = setup({
        auth: [POPUP_BLOCKED, okResult(TOKEN)],
        prompts: {[CONNECT_PROMPT]: 'connect'},
      });
      await t.run();

      expect(promptsShown()).toEqual([CONNECT_PROMPT]);
      expect(t.authenticate).toHaveBeenCalledTimes(2);
      expect(t.openTraceFromBuffer).toHaveBeenCalled();
      expectNoAlerts();
    });

    test.each([
      ['cancels', 'cancel'],
      ['dismisses', undefined],
    ])(
      'stops silently if the user %s the connect prompt',
      async (_n, choice) => {
        const t = setup({
          auth: [POPUP_BLOCKED],
          prompts: choice === undefined ? {} : {[CONNECT_PROMPT]: choice},
        });
        await t.run();

        expect(t.authenticate).toHaveBeenCalledTimes(1);
        expect(t.openFile).not.toHaveBeenCalled();
        expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
        expectNoAlerts();
      },
    );

    test('reports the failure if the retry does not succeed either', async () => {
      const error: AuthError = {type: 'popup_closed'};
      const t = setup({
        auth: [POPUP_BLOCKED, errResult(error)],
        prompts: {[CONNECT_PROMPT]: 'connect'},
      });
      await t.run();

      expect(alertConnectFailed).toHaveBeenCalledWith(error);
      expect(t.openFile).not.toHaveBeenCalled();
      expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
    });

    test('reports other failures without offering to retry', async () => {
      const error: AuthError = {type: 'missing_scopes'};
      const t = setup({auth: [errResult(error)]});
      await t.run();

      expect(promptsShown()).toEqual([]);
      expect(alertConnectFailed).toHaveBeenCalledWith(error);
      expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
    });
  });

  // A reload should retry the deep link until the user has connected, and
  // not after that.
  describe("the URL's state parameter", () => {
    test('is removed once connected, leaving other parameters', async () => {
      const t = setup();
      await t.run();

      expect(window.location.search).toBe('?other=1');
    });

    test('is removed even if the download then fails', async () => {
      const t = setup({downloads: [errResult({type: 'error'})]});
      await t.run();

      expect(window.location.search).toBe('?other=1');
    });

    test('is kept if connecting fails', async () => {
      const t = setup({auth: [errResult({type: 'popup_closed'})]});
      await t.run();

      expect(window.location.search).toBe('?state=abc&other=1');
    });

    test('is kept if the user declines to connect', async () => {
      const t = setup({auth: [POPUP_BLOCKED]});
      await t.run();

      expect(window.location.search).toBe('?state=abc&other=1');
    });
  });

  describe('when access to the file is denied', () => {
    test('asks to authorize, shows the picker, then downloads again', async () => {
      const t = setup({
        downloads: [ACCESS_DENIED, okResult(CONTENTS)],
        prompts: {[AUTHORIZE_PROMPT]: 'authorize'},
      });
      await t.run();

      expect(promptsShown()).toEqual([AUTHORIZE_PROMPT]);
      expect(t.requestFileAccess).toHaveBeenCalledWith(TOKEN, FILE_ID);
      expect(t.openFile).toHaveBeenCalledTimes(2);
      expect(t.openTraceFromBuffer).toHaveBeenCalledWith(
        expect.objectContaining({buffer: CONTENTS, shareable: false}),
      );
      expectNoAlerts();
    });

    test('names the trace after the file the user authorized', async () => {
      const t = setup({
        meta: ACCESS_DENIED,
        downloads: [ACCESS_DENIED, okResult(CONTENTS)],
        prompts: {[AUTHORIZE_PROMPT]: 'authorize'},
      });
      await t.run();

      expect(t.openTraceFromBuffer).toHaveBeenCalledWith(
        expect.objectContaining({title: 'picked.pftrace'}),
      );
    });

    test.each([
      ['cancels', 'cancel'],
      ['dismisses', undefined],
    ])(
      'stops silently if the user %s the authorize prompt',
      async (_n, choice) => {
        const t = setup({
          downloads: [ACCESS_DENIED],
          prompts: choice === undefined ? {} : {[AUTHORIZE_PROMPT]: choice},
        });
        await t.run();

        expect(t.requestFileAccess).not.toHaveBeenCalled();
        expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
        expectNoAlerts();
      },
    );

    test('skips the authorize prompt if the user turned it off', async () => {
      const t = setup({
        downloads: [ACCESS_DENIED, okResult(CONTENTS)],
        hideAuthDialog: true,
      });
      await t.run();

      expect(promptsShown()).toEqual([]);
      expect(t.requestFileAccess).toHaveBeenCalledWith(TOKEN, FILE_ID);
      expect(t.openTraceFromBuffer).toHaveBeenCalled();
    });

    test('stops silently if the picker is cancelled', async () => {
      const t = setup({
        downloads: [ACCESS_DENIED],
        picked: errResult({type: 'cancelled'}),
        hideAuthDialog: true,
      });
      await t.run();

      expect(t.openFile).toHaveBeenCalledTimes(1);
      expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
      expectNoAlerts();
    });

    test('reports a picker failure and stops', async () => {
      const error = {type: 'error', cause: 'boom'} as const;
      const t = setup({
        downloads: [ACCESS_DENIED],
        picked: errResult(error),
        hideAuthDialog: true,
      });
      await t.run();

      expect(alertPickerFailed).toHaveBeenCalledWith(error);
      expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
    });

    test('reports the failure if access is still denied afterwards', async () => {
      const t = setup({
        downloads: [ACCESS_DENIED],
        hideAuthDialog: true,
      });
      await t.run();

      expect(t.openFile).toHaveBeenCalledTimes(2);
      expect(alertDownloadFailed).toHaveBeenCalledWith(
        ACCESS_DENIED.error,
        'picked.pftrace',
      );
      expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
    });
  });

  describe('when the download fails for another reason', () => {
    test('reports it without offering the picker', async () => {
      const error: DriveError = {type: 'error', cause: 'boom'};
      const t = setup({downloads: [errResult(error)]});
      await t.run();

      expect(promptsShown()).toEqual([]);
      expect(t.requestFileAccess).not.toHaveBeenCalled();
      expect(alertDownloadFailed).toHaveBeenCalledWith(error, 'meta.pftrace');
      expect(t.openTraceFromBuffer).not.toHaveBeenCalled();
    });
  });
});
