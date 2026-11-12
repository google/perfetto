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

import type {ReadonlyNonEmpty} from '../../../base/array_utils';
import {loadScript} from '../../../base/dom_utils';
import {errResult, okResult, type Result} from '../../../base/result';

// The Google Identity Services script, loaded once by
// GoogleOAuthClient.create().
const GSI_CLIENT_URL = 'https://accounts.google.com/gsi/client';

// How long to wait for that script before giving up. Without this a stalled
// request would never settle.
const LOAD_TIMEOUT_MS = 30000;

// Why getting an access token did not succeed.
export interface AuthError {
  // 'missing_scopes' means the user connected but did not grant every scope
  // that was asked for. Google lets users untick scopes when an app asks for
  // more than one at a time.
  readonly type: 'popup_blocked' | 'popup_closed' | 'missing_scopes' | 'error';
  readonly cause?: unknown;
}

// The most recent access token, and what it is good for.
interface CachedToken {
  readonly accessToken: string;
  readonly expiresAtMs: number;
  // The raw GIS response, passed to hasGrantedAllScopes() on cache checks.
  readonly tokenResponse: GDriveTokenResponse;
}

// Obtains OAuth access tokens for the user's Google account, through Google
// Identity Services, and holds the current one in memory. It knows nothing
// about what the token is used for.
export class GoogleOAuthClient {
  // The requests in flight, keyed by the scope they ask for.
  private readonly pending = new Map<
    string,
    Promise<Result<string, AuthError>>
  >();
  private cached: CachedToken | undefined;

  private constructor(private readonly clientId: string) {}

  // Loads Google Identity Services and returns a ready-to-use client. Rejects
  // if it cannot be loaded.
  //
  // Nothing here opens a popup or needs a user gesture; only authenticate()
  // does.
  static async create(args: {
    readonly clientId: string;
  }): Promise<GoogleOAuthClient> {
    await loadScript(GSI_CLIENT_URL, LOAD_TIMEOUT_MS);
    if (window.google?.accounts?.oauth2 === undefined) {
      throw new Error('Google Identity Services is not available');
    }
    return new GoogleOAuthClient(args.clientId);
  }

  // Returns an access token that covers `scopes`, asking the user to connect
  // their Google account if there is no valid token that does. That opens a
  // Google popup, which browsers only allow from a user gesture, so nothing is
  // awaited here before the popup is requested.
  //
  // Ask only for what the action at hand needs. Google shows tick boxes when
  // several scopes are requested together, and the user can refuse any of
  // them; a single scope is simply allowed or cancelled.
  //
  // This never rejects: every failure is returned via Result.
  authenticate(
    scopes: ReadonlyNonEmpty<string>,
  ): Promise<Result<string, AuthError>> {
    const cached = this.cached;
    const oauth2 = window.google?.accounts?.oauth2;
    if (
      cached !== undefined &&
      Date.now() < cached.expiresAtMs &&
      oauth2?.hasGrantedAllScopes(cached.tokenResponse, ...scopes) === true
    ) {
      return Promise.resolve(okResult(cached.accessToken));
    }

    // Share a single request between concurrent callers asking for the same
    // thing, and forget it once it settles so that a later call can try again.
    const scopeKey = [...scopes].sort().join(' ');
    let request = this.pending.get(scopeKey);
    if (request === undefined) {
      request = this.requestToken(scopes).finally(() => {
        this.pending.delete(scopeKey);
      });
      this.pending.set(scopeKey, request);
    }
    return request;
  }

  // Asks Google for a new token covering `scopes`. Like authenticate(), this
  // never rejects.
  private requestToken(
    scopes: ReadonlyNonEmpty<string>,
  ): Promise<Result<string, AuthError>> {
    return new Promise<Result<string, AuthError>>((resolve) => {
      const fail = (cause: unknown) =>
        resolve(errResult({type: 'error', cause}));

      // The google.accounts.oauth2 token client is typed via the Window
      // augmentation in ./types/google_identity.d.ts.
      const oauth2 = window.google?.accounts?.oauth2;
      if (oauth2 === undefined) {
        fail('Google Identity Services is not available');
        return;
      }

      // Google's library is not expected to throw, but if it did this promise
      // would reject, so report that as an error like any other.
      try {
        const tokenClient: GDriveTokenClient = oauth2.initTokenClient({
          client_id: this.clientId,
          scope: scopes.join(' '),
          callback: (tokenResponse: GDriveTokenResponse) => {
            const error = tokenResponse.error;
            if (typeof error === 'string' && error !== '') {
              fail(error);
              return;
            }
            const accessToken = tokenResponse.access_token;
            if (typeof accessToken !== 'string') {
              fail('No access token returned');
              return;
            }
            // Google reports the scopes it actually granted. That can be
            // fewer than were asked for, if the user unticked some.
            if (!oauth2.hasGrantedAllScopes(tokenResponse, ...scopes)) {
              resolve(errResult({type: 'missing_scopes'}));
              return;
            }
            const parsedExpires = Number(tokenResponse.expires_in);
            const expiresInSec = Number.isFinite(parsedExpires)
              ? parsedExpires
              : 3600;
            this.cached = {
              accessToken,
              expiresAtMs: Date.now() + Math.max(0, expiresInSec - 60) * 1000,
              tokenResponse,
            };
            resolve(okResult(accessToken));
          },
          error_callback: (error: GDriveTokenClientError) => {
            if (error.type === 'popup_failed_to_open') {
              resolve(errResult({type: 'popup_blocked'}));
            } else if (error.type === 'popup_closed') {
              resolve(errResult({type: 'popup_closed'}));
            } else {
              fail(`Google reported an error: ${error.type}`);
            }
          },
        });
        tokenClient.requestAccessToken({prompt: ''});
      } catch (e) {
        fail(e);
      }
    });
  }
}
