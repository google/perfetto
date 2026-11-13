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

import {
  DriveClient,
  isValidDriveId,
  parseGDriveStateParam,
} from './drive_client';

// DriveClient.create() loads Google's scripts before returning a client.
// Replace the loaders so a client can be built without the network.
vi.mock('../../../base/dom_utils', () => ({loadScript: vi.fn()}));
vi.mock('./gapi', () => ({loadGapiModules: vi.fn()}));

async function createClient(): Promise<DriveClient> {
  return await DriveClient.create({apiKey: 'key', appId: 'app'});
}

type PickerCallback = (data: google.picker.ResponseObject) => void;

// One picker dialog shown to the user. respond() answers it as the real picker
// would, by calling the callback it was built with.
interface ShownPicker {
  readonly respond: PickerCallback;
  // The file the picker was limited to, if any.
  readonly fileIds?: string;
}

// Installs a stand-in for google.picker, and returns the list of pickers shown
// through it. Nothing is answered automatically: each test plays the user, by
// calling respond().
//
// This encodes how we believe the real picker behaves: that it reports what
// the user did through the callback, using the actions in google.picker.Action.
function installFakePicker(
  opts: {readonly buildThrows?: unknown} = {},
): ShownPicker[] {
  const shown: ShownPicker[] = [];

  class DocsView {
    fileIds?: string;
    setMode() {
      return this;
    }
    setFileIds(fileIds: string) {
      this.fileIds = fileIds;
      return this;
    }
  }

  class PickerBuilder {
    private callback: PickerCallback = () => {};
    private view?: DocsView;
    setAppId() {
      return this;
    }
    setOAuthToken() {
      return this;
    }
    setDeveloperKey() {
      return this;
    }
    setOrigin() {
      return this;
    }
    addView(view: DocsView) {
      this.view = view;
      return this;
    }
    setTitle() {
      return this;
    }
    setCallback(callback: PickerCallback) {
      this.callback = callback;
      return this;
    }
    build() {
      if (opts.buildThrows !== undefined) throw opts.buildThrows;
      return {
        setVisible: () => {
          shown.push({respond: this.callback, fileIds: this.view?.fileIds});
        },
      };
    }
  }

  vi.stubGlobal('google', {
    picker: {
      Action: {PICKED: 'picked', CANCEL: 'cancel', ERROR: 'error'},
      ViewId: {DOCS: 'docs', FOLDERS: 'folders'},
      DocsViewMode: {LIST: 'list'},
      DocsView,
      PickerBuilder,
    },
  });
  return shown;
}

function latest(shown: ShownPicker[]): ShownPicker {
  const picker = shown.at(-1);
  if (picker === undefined) throw new Error('No picker was shown');
  return picker;
}

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

describe('parseGDriveStateParam', () => {
  test('parses valid state with file ID only', () => {
    expect(
      parseGDriveStateParam(
        JSON.stringify({ids: ['file_123'], action: 'open'}),
      ),
    ).toEqual({
      ok: true,
      value: {fileId: 'file_123', resourceKey: undefined},
    });
  });

  test('parses valid state with matching resource key', () => {
    expect(
      parseGDriveStateParam(
        JSON.stringify({
          ids: ['file_123', 'other_456'],
          resourceKeys: {file_123: 'rk-ABC_9'},
        }),
      ),
    ).toEqual({
      ok: true,
      value: {fileId: 'file_123', resourceKey: 'rk-ABC_9'},
    });
  });

  test('ignores resource keys for other file IDs', () => {
    expect(
      parseGDriveStateParam(
        JSON.stringify({
          ids: ['file_123'],
          resourceKeys: {other_456: 'bad/key'},
        }),
      ),
    ).toEqual({
      ok: true,
      value: {fileId: 'file_123', resourceKey: undefined},
    });
  });

  test('rejects invalid JSON', () => {
    expect(parseGDriveStateParam('not-json')).toEqual({
      ok: false,
      error: 'Invalid JSON',
    });
  });

  test('rejects payloads not matching schema', () => {
    expect(parseGDriveStateParam(JSON.stringify({ids: 'file_123'}))).toEqual({
      ok: false,
      error: 'Invalid GDrive state',
    });
    expect(parseGDriveStateParam(JSON.stringify([]))).toEqual({
      ok: false,
      error: 'Invalid GDrive state',
    });
  });

  test('rejects missing or empty file IDs', () => {
    expect(parseGDriveStateParam(JSON.stringify({}))).toEqual({
      ok: false,
      error: 'No file IDs provided',
    });
    expect(parseGDriveStateParam(JSON.stringify({ids: []}))).toEqual({
      ok: false,
      error: 'No file IDs provided',
    });
    expect(parseGDriveStateParam(JSON.stringify({ids: ['']}))).toEqual({
      ok: false,
      error: 'No file IDs provided',
    });
  });

  test('rejects invalid file ID', () => {
    expect(parseGDriveStateParam(JSON.stringify({ids: ['../secret']}))).toEqual(
      {
        ok: false,
        error: 'Invalid file ID',
      },
    );
  });

  test('rejects invalid resource key', () => {
    expect(
      parseGDriveStateParam(
        JSON.stringify({
          ids: ['file_123'],
          resourceKeys: {file_123: 'bad\r\nheader'},
        }),
      ),
    ).toEqual({
      ok: false,
      error: 'Invalid resource key',
    });
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

  // The resource key is sent in a request header, so it is held to the same
  // rules as a file ID.
  test.each(['', 'a/b', 'abc\r\nX: y', 'abc def'])(
    'rejects the invalid resource key %j without touching the network',
    async (resourceKey) => {
      const client = await createClient();
      const result = await client.openFile('token', 'file123', resourceKey);

      expect(result).toMatchObject({ok: false, error: {type: 'invalid_id'}});
      expect(fetchMock).not.toHaveBeenCalled();
    },
  );

  test('sends a resource key to Drive alongside the file it is for', async () => {
    const client = await createClient();
    const result = await client.openFile('token', 'file123', 'rk-1');

    expect(result.ok).toBe(true);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [input, init] = fetchMock.mock.calls[0]!;
    const request = new Request(input, init);
    expect(request.headers.get('X-Goog-Drive-Resource-Keys')).toBe(
      'file123/rk-1',
    );
  });

  test('sends no resource key header when there is no key', async () => {
    const client = await createClient();
    await client.openFile('token', 'file123');

    const [input, init] = fetchMock.mock.calls[0]!;
    const request = new Request(input, init);
    expect(request.headers.has('X-Goog-Drive-Resource-Keys')).toBe(false);
  });
});

describe('DriveClient.getMeta', () => {
  const fetchMock = vi.fn();

  beforeEach(() => {
    fetchMock.mockReset();
    fetchMock.mockResolvedValue(Response.json({name: 'my.pftrace'}));
    vi.stubGlobal('fetch', fetchMock);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  test("returns the file's name", async () => {
    const client = await createClient();
    const result = await client.getMeta('token', 'file123');

    expect(result).toEqual({ok: true, value: {name: 'my.pftrace'}});
  });

  test.each([
    ['has no name', Response.json({})],
    ['has a name that is not a string', Response.json({name: 42})],
    ['is not JSON', new Response('<html>')],
  ])('reports an error if the response %s', async (_name, response) => {
    fetchMock.mockResolvedValue(response);
    const client = await createClient();
    const result = await client.getMeta('token', 'file123');

    expect(result).toMatchObject({ok: false, error: {type: 'error'}});
  });

  test.each([403, 404])('reports HTTP %i as access denied', async (status) => {
    fetchMock.mockResolvedValue(new Response(null, {status}));
    const client = await createClient();
    const result = await client.getMeta('token', 'file123');

    expect(result).toMatchObject({ok: false, error: {type: 'access_denied'}});
  });

  test('reports a network failure as an error, without rejecting', async () => {
    fetchMock.mockRejectedValue(new TypeError('Failed to fetch'));
    const client = await createClient();
    const result = await client.getMeta('token', 'file123');

    expect(result).toMatchObject({ok: false, error: {type: 'error'}});
  });

  test.each([
    ['file ID', '../other', undefined],
    ['resource key', 'file123', 'a/b'],
  ])(
    'rejects an invalid %s without touching the network',
    async (_name, fileId, resourceKey) => {
      const client = await createClient();
      const result = await client.getMeta('token', fileId, resourceKey);

      expect(result).toMatchObject({ok: false, error: {type: 'invalid_id'}});
      expect(fetchMock).not.toHaveBeenCalled();
    },
  );
});

describe('DriveClient.pickFile', () => {
  const FILE = {id: 'file123', name: 'my.pftrace'};

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  test('returns the file the user picks', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    const pending = client.pickFile('token');

    expect(shown).toHaveLength(1);
    latest(shown).respond({action: 'picked', docs: [FILE]});

    expect(await pending).toEqual({ok: true, value: FILE});
  });

  test('returns the first file if several are picked', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    const pending = client.pickFile('token');
    latest(shown).respond({
      action: 'picked',
      docs: [FILE, {id: 'other456'}],
    });

    expect(await pending).toEqual({ok: true, value: FILE});
  });

  test('Closing the picker reports cancelled', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    const pending = client.pickFile('token');
    latest(shown).respond({action: 'cancel'});

    expect(await pending).toMatchObject({
      ok: false,
      error: {type: 'cancelled'},
    });
  });

  test('reports an error from the picker', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    const pending = client.pickFile('token');
    latest(shown).respond({action: 'error'});

    expect(await pending).toMatchObject({ok: false, error: {type: 'error'}});
  });

  // The picker also reports progress, such as having loaded. Those must not
  // be mistaken for the user's answer.
  test('keeps waiting through actions that are not an answer', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    const settled = vi.fn();
    const pending = client.pickFile('token');
    void pending.then(settled);

    latest(shown).respond({action: 'loaded'});
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(settled).not.toHaveBeenCalled();

    latest(shown).respond({action: 'picked', docs: [FILE]});
    expect(await pending).toEqual({ok: true, value: FILE});
  });

  test('reports rather than rejects if the picker cannot be shown', async () => {
    const failure = new Error('boom');
    const shown = installFakePicker({buildThrows: failure});
    const client = await createClient();

    expect(await client.pickFile('token')).toMatchObject({
      ok: false,
      error: {type: 'error', cause: failure},
    });
    expect(shown).toHaveLength(0);
  });

  test('throws if picked doc array is empty', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    client.pickFile('fakeToken');

    expect(() => {
      latest(shown).respond({action: 'picked', docs: []});
    }).toThrow();
  });

  test('throws if picked doc array is missing', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    client.pickFile('fakeToken');

    expect(() => {
      latest(shown).respond({action: 'picked'});
    }).toThrow();
  });
});

describe('DriveClient.requestFileAccess', () => {
  const FILE = {id: 'file123', name: 'my.pftrace'};

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  test('shows a picker limited to the file, and returns it once picked', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    const pending = client.requestFileAccess('token', 'file123');

    expect(shown).toHaveLength(1);
    expect(latest(shown).fileIds).toBe('file123');

    latest(shown).respond({action: 'picked', docs: [FILE]});
    expect(await pending).toEqual({ok: true, value: FILE});
  });

  test('reports a cancellation if the user closes the picker', async () => {
    const shown = installFakePicker();
    const client = await createClient();
    const pending = client.requestFileAccess('token', 'file123');
    latest(shown).respond({action: 'cancel'});

    expect(await pending).toMatchObject({
      ok: false,
      error: {type: 'cancelled'},
    });
  });

  test.each(['', '../other', 'abc/def', 'abc def'])(
    'rejects the invalid file ID %j without showing a picker',
    async (fileId) => {
      const shown = installFakePicker();
      const client = await createClient();
      const result = await client.requestFileAccess('token', fileId);

      expect(result).toMatchObject({ok: false, error: {type: 'invalid_id'}});
      expect(shown).toHaveLength(0);
    },
  );
});
