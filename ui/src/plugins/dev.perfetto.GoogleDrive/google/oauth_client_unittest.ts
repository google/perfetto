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

import {GoogleOAuthClient} from './oauth_client';

// GoogleOAuthClient.create() loads Google's script before returning a client.
// Replace the loader so a client can be built without the network.
vi.mock('../../../base/dom_utils', () => ({loadScript: vi.fn()}));

const SCOPE_A = 'https://example.com/auth/a';
const SCOPE_B = 'https://example.com/auth/b';

// The part of window.google that Google Identity Services provides. Other
// Google libraries add to window.google too, so the tests set it through this
// narrower type.
interface GisWindow {
  google?: {accounts?: {oauth2?: GisOauth2}};
}
const gisWindow: GisWindow = window;

// One request to show Google's popup, and the callbacks to answer it with.
interface PopupRequest {
  readonly scope: string;
  readonly callback?: (response: GisTokenResponse) => void;
  readonly error_callback?: (error: GisTokenClientError) => void;
}

// Installs a stand-in for Google Identity Services, and returns the list of
// popups requested through it. Nothing is answered automatically: each test
// plays the user, with grant() or fail().
//
// This encodes how we believe the real library behaves: that it answers
// through these callbacks, and that hasGrantedAllScopes() compares against the
// response's space separated `scope`.
function installFakeGis(): PopupRequest[] {
  const popups: PopupRequest[] = [];
  gisWindow.google = {
    accounts: {
      oauth2: {
        initTokenClient: (params) => ({
          requestAccessToken: () => {
            popups.push(params);
          },
        }),
        hasGrantedAllScopes: (response, ...scopes) => {
          const granted = (response.scope ?? '').split(' ');
          return scopes.every((scope) => granted.includes(scope));
        },
      },
    },
  };
  return popups;
}

function latest(popups: PopupRequest[]): PopupRequest {
  const popup = popups.at(-1);
  if (popup === undefined) throw new Error('No Google popup was requested');
  return popup;
}

// Answers a popup as if the user allowed everything that was asked for.
function grant(
  popup: PopupRequest,
  overrides: Partial<GisTokenResponse> = {},
): void {
  popup.callback?.({
    access_token: 'token',
    expires_in: '3600',
    scope: popup.scope,
    ...overrides,
  });
}

// Answers a popup as if it could not be shown or completed.
function fail(popup: PopupRequest, type: string): void {
  popup.error_callback?.({type});
}

async function createClient(): Promise<GoogleOAuthClient> {
  return await GoogleOAuthClient.create({clientId: 'client'});
}

describe('GoogleOAuthClient', () => {
  let popups: PopupRequest[];

  beforeEach(() => {
    popups = installFakeGis();
  });

  afterEach(() => {
    gisWindow.google = undefined;
    vi.useRealTimers();
  });

  describe('create', () => {
    test('rejects if Google Identity Services did not load', async () => {
      gisWindow.google = undefined;
      await expect(createClient()).rejects.toThrow();
    });
  });

  describe('authenticate', () => {
    test('returns the token once the user connects', async () => {
      const client = await createClient();
      const pending = client.authenticate([SCOPE_A]);
      grant(latest(popups), {access_token: 'abc'});

      expect(await pending).toEqual({ok: true, value: 'abc'});
    });

    // Browsers only allow a popup while handling a click, so it must be
    // requested before authenticate() first yields.
    test('requests the popup synchronously', async () => {
      const client = await createClient();
      const pending = client.authenticate([SCOPE_A]);

      expect(popups).toHaveLength(1);

      grant(latest(popups));
      await pending;
    });

    test('asks for all the scopes together', async () => {
      const client = await createClient();
      const pending = client.authenticate([SCOPE_A, SCOPE_B]);

      expect(latest(popups).scope.split(' ').sort()).toEqual(
        [SCOPE_A, SCOPE_B].sort(),
      );

      grant(latest(popups));
      await pending;
    });

    describe('caching', () => {
      test('reuses the token without another popup', async () => {
        const client = await createClient();
        const first = client.authenticate([SCOPE_A]);
        grant(latest(popups), {access_token: 'abc'});
        await first;

        const second = await client.authenticate([SCOPE_A]);

        expect(second).toEqual({ok: true, value: 'abc'});
        expect(popups).toHaveLength(1);
      });

      test('reuses a token that covers more than is asked for', async () => {
        const client = await createClient();
        const first = client.authenticate([SCOPE_A, SCOPE_B]);
        grant(latest(popups));
        await first;

        const second = await client.authenticate([SCOPE_B]);

        expect(second.ok).toBe(true);
        expect(popups).toHaveLength(1);
      });

      test('asks again for a scope the token does not cover', async () => {
        const client = await createClient();
        const first = client.authenticate([SCOPE_A]);
        grant(latest(popups));
        await first;

        const second = client.authenticate([SCOPE_B]);

        expect(popups).toHaveLength(2);
        expect(latest(popups).scope).toBe(SCOPE_B);

        grant(latest(popups));
        await second;
      });

      test('asks again once the token has expired', async () => {
        vi.useFakeTimers();
        const client = await createClient();
        const first = client.authenticate([SCOPE_A]);
        grant(latest(popups), {expires_in: '3600'});
        await first;

        vi.advanceTimersByTime(3600 * 1000);
        const second = client.authenticate([SCOPE_A]);

        expect(popups).toHaveLength(2);

        grant(latest(popups));
        await second;
      });

      // A token about to expire could do so before the request it is used
      // for reaches Google.
      test('stops reusing the token shortly before it expires', async () => {
        vi.useFakeTimers();
        const client = await createClient();
        const first = client.authenticate([SCOPE_A]);
        grant(latest(popups), {expires_in: '3600'});
        await first;

        vi.advanceTimersByTime(3500 * 1000);
        await client.authenticate([SCOPE_A]);
        expect(popups).toHaveLength(1);

        vi.advanceTimersByTime(99 * 1000);
        const late = client.authenticate([SCOPE_A]);
        expect(popups).toHaveLength(2);

        grant(latest(popups));
        await late;
      });
    });

    describe('concurrent calls', () => {
      test('share one popup when asking for the same scopes', async () => {
        const client = await createClient();
        const first = client.authenticate([SCOPE_A, SCOPE_B]);
        const second = client.authenticate([SCOPE_B, SCOPE_A]);

        expect(popups).toHaveLength(1);

        grant(latest(popups), {access_token: 'abc'});
        expect(await first).toEqual({ok: true, value: 'abc'});
        expect(await second).toEqual({ok: true, value: 'abc'});
      });

      test('try again after a shared request fails', async () => {
        const client = await createClient();
        const first = client.authenticate([SCOPE_A]);
        fail(latest(popups), 'popup_closed');
        await first;

        const second = client.authenticate([SCOPE_A]);

        expect(popups).toHaveLength(2);

        grant(latest(popups));
        expect((await second).ok).toBe(true);
      });
    });

    describe('failures', () => {
      test.each([
        ['popup_failed_to_open', 'popup_blocked'],
        ['popup_closed', 'popup_closed'],
        ['unknown', 'error'],
      ])('reports the popup error %j as %j', async (gisError, expected) => {
        const client = await createClient();
        const pending = client.authenticate([SCOPE_A]);
        fail(latest(popups), gisError);

        expect(await pending).toMatchObject({
          ok: false,
          error: {type: expected},
        });
      });

      test('reports an error response from Google', async () => {
        const client = await createClient();
        const pending = client.authenticate([SCOPE_A]);
        latest(popups).callback?.({error: 'access_denied'});

        expect(await pending).toMatchObject({
          ok: false,
          error: {type: 'error', cause: 'access_denied'},
        });
      });

      test('reports a response with no token', async () => {
        const client = await createClient();
        const pending = client.authenticate([SCOPE_A]);
        latest(popups).callback?.({scope: SCOPE_A});

        expect(await pending).toMatchObject({
          ok: false,
          error: {type: 'error'},
        });
      });

      test('reports when the user does not grant every scope', async () => {
        const client = await createClient();
        const pending = client.authenticate([SCOPE_A, SCOPE_B]);
        grant(latest(popups), {scope: SCOPE_A});

        expect(await pending).toMatchObject({
          ok: false,
          error: {type: 'missing_scopes'},
        });
      });

      test('does not keep a token that is missing scopes', async () => {
        const client = await createClient();
        const first = client.authenticate([SCOPE_A, SCOPE_B]);
        grant(latest(popups), {scope: SCOPE_A});
        await first;

        // The token did cover this scope, but it was never accepted.
        const second = client.authenticate([SCOPE_A]);

        expect(popups).toHaveLength(2);

        grant(latest(popups));
        await second;
      });

      test('reports rather than rejects if Google throws', async () => {
        const failure = new Error('boom');
        gisWindow.google = {
          accounts: {
            oauth2: {
              initTokenClient: () => {
                throw failure;
              },
              hasGrantedAllScopes: () => true,
            },
          },
        };
        const client = await createClient();

        expect(await client.authenticate([SCOPE_A])).toMatchObject({
          ok: false,
          error: {type: 'error', cause: failure},
        });
      });
    });
  });
});
