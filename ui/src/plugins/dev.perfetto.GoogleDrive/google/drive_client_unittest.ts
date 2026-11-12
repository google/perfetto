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

import {DriveClient, isValidDriveId} from './drive_client';

// DriveClient.create() loads Google's scripts before returning a client.
// Replace the loaders so a client can be built without the network.
vi.mock('../../../base/dom_utils', () => ({loadScript: vi.fn()}));
vi.mock('./gapi', () => ({loadGapiModules: vi.fn()}));

describe('isValidDriveId', () => {
  test('accepts valid drive IDs and resource keys', () => {
    expect(isValidDriveId('abc123')).toBe(true);
    expect(isValidDriveId('0-aZ_9')).toBe(true);
    expect(isValidDriveId('1BxiMVs0XRA5nFMdKvBdBZjgmUUqptlbs74OgvE2upms')).toBe(
      true,
    );
  });

  test('rejects empty string', () => {
    expect(isValidDriveId('')).toBe(false);
  });

  test('rejects path traversal', () => {
    expect(isValidDriveId('..')).toBe(false);
    expect(isValidDriveId('../other')).toBe(false);
    expect(isValidDriveId('abc/def')).toBe(false);
    expect(isValidDriveId('abc\\def')).toBe(false);
    expect(isValidDriveId('..%2Fother')).toBe(false);
  });

  test('rejects query and fragment injection', () => {
    expect(isValidDriveId('abc?fields=*')).toBe(false);
    expect(isValidDriveId('abc&alt=media')).toBe(false);
    expect(isValidDriveId('abc#fragment')).toBe(false);
  });

  test('rejects header injection and whitespace', () => {
    expect(isValidDriveId('abc\r\nX-Injected: true')).toBe(false);
    expect(isValidDriveId('abc\ndef')).toBe(false);
    expect(isValidDriveId('abc def')).toBe(false);
  });
});

describe('DriveClient.openFile', () => {
  const fetchMock = vi.fn();

  beforeEach(() => {
    fetchMock.mockReset();
    fetchMock.mockResolvedValue(new Response(new ArrayBuffer(8)));
    vi.stubGlobal('fetch', fetchMock);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  async function createClient(): Promise<DriveClient> {
    return await DriveClient.create({apiKey: 'key', appId: 'app'});
  }

  test.each(['', '../other', 'abc/def', 'abc?alt=media', 'abc\r\nX: y'])(
    'rejects the invalid file ID %j without touching the network',
    async (fileId) => {
      const client = await createClient();
      const result = await client.openFile('token', fileId);

      expect(result).toMatchObject({ok: false, error: {type: 'invalid_id'}});
      expect(fetchMock).not.toHaveBeenCalled();
    },
  );

  // Without this, the test above would still pass if openFile() stopped using
  // fetch altogether.
  test('does reach the network for a valid file ID', async () => {
    const client = await createClient();
    const result = await client.openFile('token', 'file123');

    expect(result.ok).toBe(true);
    expect(fetchMock).toHaveBeenCalledTimes(1);
  });

  test("returns the file's contents", async () => {
    fetchMock.mockResolvedValue(new Response(new Uint8Array([1, 2, 3])));
    const client = await createClient();
    const result = await client.openFile('token', 'file123');

    expect(result.ok).toBe(true);
    expect(Array.from(new Uint8Array(result.value!))).toEqual([1, 2, 3]);
  });

  // The flows offer the file picker when access is denied, so this mapping
  // decides whether the user is given a way to recover.
  test.each([403, 404])('reports HTTP %i as access denied', async (status) => {
    fetchMock.mockResolvedValue(new Response(null, {status}));
    const client = await createClient();
    const result = await client.openFile('token', 'file123');

    expect(result).toMatchObject({ok: false, error: {type: 'access_denied'}});
  });

  test.each([400, 401, 429, 500, 503])(
    'reports HTTP %i as a generic error',
    async (status) => {
      fetchMock.mockResolvedValue(new Response(null, {status}));
      const client = await createClient();
      const result = await client.openFile('token', 'file123');

      expect(result).toMatchObject({ok: false, error: {type: 'error'}});
    },
  );

  test('reports a network failure as an error, without rejecting', async () => {
    const failure = new TypeError('Failed to fetch');
    fetchMock.mockRejectedValue(failure);
    const client = await createClient();
    const result = await client.openFile('token', 'file123');

    expect(result).toMatchObject({
      ok: false,
      error: {type: 'error', cause: failure},
    });
  });

  test('reports a failure to read the body as an error, without rejecting', async () => {
    const failure = new Error('connection lost');
    const body = new ReadableStream({
      start: (controller) => controller.error(failure),
    });
    fetchMock.mockResolvedValue(new Response(body));
    const client = await createClient();
    const result = await client.openFile('token', 'file123');

    expect(result).toMatchObject({ok: false, error: {type: 'error'}});
  });

  test('sends the token to Google only, and only in a header', async () => {
    const client = await createClient();
    await client.openFile('secret-token', 'file123');

    // Normalize however fetch() was called into a Request, so this does not
    // depend on the shape of the arguments.
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [input, init] = fetchMock.mock.calls[0]!;
    const request = new Request(input, init);

    expect(new URL(request.url).origin).toBe('https://www.googleapis.com');
    expect(request.headers.get('Authorization')).toBe('Bearer secret-token');
    expect(request.url).not.toContain('secret-token');
  });
});
