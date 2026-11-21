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

import {errResult, okResult, type Result} from '../../base/result';
import type {Trace} from '../../public/trace';
import type {
  DriveClient,
  DriveError,
  DrivePermission,
  PermissionError,
  UploadedFile,
  UploadError,
} from './google/drive_client';
import type {GoogleOAuthClient} from './google/oauth_client';
import {uploadTraceToGDrive} from './share_to_gdrive';
import type {ShareState} from './views/share_form';

const TOKEN = 'token';
const UPLOADED: UploadedFile = {id: 'file123', resourceKey: 'rk1'};

interface SetupOptions {
  readonly shareWithOrgOrAnyone?: boolean;
  readonly upload?: Result<UploadedFile, UploadError>;
  readonly userEmail?: Result<string, DriveError>;
  readonly createPermission?: (
    token: string,
    fileId: string,
    permission: DrivePermission,
  ) => Promise<Result<void, PermissionError>>;
}

function setup(opts: SetupOptions = {}) {
  const authenticate = vi.fn(async () => okResult(TOKEN));
  const uploadFile = vi.fn(async () => opts.upload ?? okResult(UPLOADED));
  const getUserEmail = vi.fn(
    async () => opts.userEmail ?? okResult('alice@example.com'),
  );
  const createPermission = vi.fn(
    opts.createPermission ?? (async () => okResult()),
  );
  const getTraceFile = vi.fn(async () => new Blob(['TRACE']));
  const scheduleFullRedraw = vi.fn();

  const share: ShareState = {
    fileName: 'trace.pftrace',
    location: 'root',
    shareWithOrgOrAnyone: opts.shareWithOrgOrAnyone ?? true,
    fileId: undefined,
    resourceKey: undefined,
    upload: undefined,
    error: undefined,
  };

  const run = () =>
    uploadTraceToGDrive(
      {
        getTraceFile,
        raf: {scheduleFullRedraw},
      } as unknown as Trace,
      {authenticate} as unknown as GoogleOAuthClient,
      {uploadFile, getUserEmail, createPermission} as unknown as DriveClient,
      share,
    );

  return {run, share, uploadFile, getUserEmail, createPermission};
}

describe('uploadTraceToGDrive', () => {
  test('shares with the organization and enables search discovery', async () => {
    const t = setup({userEmail: okResult('Alice@Example.COM')});
    await t.run();

    expect(t.share.fileId).toBe(UPLOADED.id);
    expect(t.share.resourceKey).toBe(UPLOADED.resourceKey);
    expect(t.share.error).toBeUndefined();
    expect(t.createPermission).toHaveBeenCalledTimes(1);
    expect(t.createPermission).toHaveBeenCalledWith(TOKEN, UPLOADED.id, {
      type: 'domain',
      domain: 'example.com',
      role: 'reader',
      allowFileDiscovery: true,
    });
  });

  test.each(['bob@gmail.com', 'bob@googlemail.com'])(
    'shares with anyone when the consumer account is %s',
    async (email) => {
      const t = setup({userEmail: okResult(email)});
      await t.run();

      expect(t.share.fileId).toBe(UPLOADED.id);
      expect(t.share.error).toBeUndefined();
      expect(t.createPermission).toHaveBeenCalledTimes(1);
      expect(t.createPermission).toHaveBeenCalledWith(TOKEN, UPLOADED.id, {
        type: 'anyone',
        role: 'reader',
        allowFileDiscovery: true,
      });
    },
  );

  test('falls back to anyone when a non-Gmail domain is rejected as bad_request', async () => {
    const t = setup({
      userEmail: okResult('bob@yahoo.com'),
      createPermission: async (_token, _fileId, permission) =>
        permission.type === 'domain'
          ? errResult({type: 'bad_request', cause: 'Invalid domain'})
          : okResult(),
    });
    await t.run();

    expect(t.share.fileId).toBe(UPLOADED.id);
    expect(t.share.error).toBeUndefined();
    expect(t.createPermission.mock.calls).toEqual([
      [
        TOKEN,
        UPLOADED.id,
        {
          type: 'domain',
          domain: 'yahoo.com',
          role: 'reader',
          allowFileDiscovery: true,
        },
      ],
      [
        TOKEN,
        UPLOADED.id,
        {
          type: 'anyone',
          role: 'reader',
          allowFileDiscovery: true,
        },
      ],
    ]);
  });

  test('does not fall back to anyone on access_denied for domain sharing', async () => {
    const t = setup({
      userEmail: okResult('alice@example.com'),
      createPermission: async () =>
        errResult({type: 'access_denied', cause: 'Sharing disabled by admin'}),
    });
    await t.run();

    expect(t.share.fileId).toBe(UPLOADED.id);
    expect(t.createPermission).toHaveBeenCalledTimes(1);
    expect(t.share.error).toMatchObject({
      type: 'access_denied',
      cause: 'Sharing disabled by admin',
    });
  });

  test('keeps the uploaded file and records an error if getUserEmail fails', async () => {
    const t = setup({
      userEmail: errResult({type: 'error', cause: 'Network error'}),
    });
    await t.run();

    expect(t.share.fileId).toBe(UPLOADED.id);
    expect(t.createPermission).not.toHaveBeenCalled();
    expect(t.share.error).toMatchObject({
      type: 'error',
      cause: 'Network error',
    });
  });

  test('does not modify permissions when shareWithOrgOrAnyone is unchecked', async () => {
    const t = setup({shareWithOrgOrAnyone: false});
    await t.run();

    expect(t.share.fileId).toBe(UPLOADED.id);
    expect(t.share.error).toBeUndefined();
    expect(t.getUserEmail).not.toHaveBeenCalled();
    expect(t.createPermission).not.toHaveBeenCalled();
  });
});
