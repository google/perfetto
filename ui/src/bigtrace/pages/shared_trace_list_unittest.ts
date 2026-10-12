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

import {describe, expect, test, vi} from 'vitest';
import {
  formatSharedTraceListSql,
  handleSharedTraceListPostMessage,
  parseSharedTraceListMessage,
} from './shared_trace_list';

describe('shared trace list (postMessage)', () => {
  test('only a non-empty traceUuids list is accepted', () => {
    expect(parseSharedTraceListMessage('PING')).toBeUndefined();
    expect(parseSharedTraceListMessage({traceUuids: []})).toBeUndefined();
    expect(
      parseSharedTraceListMessage({traceUuids: ['u1'], title: 42}),
    ).toBeUndefined();
    expect(
      parseSharedTraceListMessage({traceUuids: ['u1', ' u1 ', ''], title: ' '}),
    ).toEqual({traceUuids: ['u1'], title: ''});
    // Line breaks would escape the `--` comment header: collapse them.
    expect(
      parseSharedTraceListMessage({traceUuids: ['u1'], title: 'a \r\n b\nc\n'}),
    ).toEqual({traceUuids: ['u1'], title: 'a b c'});
  });

  test('SQL is prefixed with title and source comments', () => {
    expect(
      formatSharedTraceListSql({
        traceUuids: ['u1'],
        sourceUrl: 'https://example.com/traces',
        title: 'Sample traces',
        perfettoSql: 'SELECT 1',
      }),
    ).toBe(
      '-- Sample traces\n-- Source: https://example.com/traces\n\nSELECT 1\n',
    );
    expect(formatSharedTraceListSql({traceUuids: ['u1']})).toBe('');
  });

  test('PING from the opener is answered with PONG; others are ignored', () => {
    const sent: unknown[][] = [];
    const opener = {postMessage: (...args: unknown[]) => sent.push(args)};
    vi.stubGlobal('opener', opener);
    const event = (source: unknown, data: unknown) =>
      ({source, data, origin: 'https://example.com'}) as MessageEvent;

    expect(handleSharedTraceListPostMessage(event(opener, 'PING'))).toBe(
      undefined,
    );
    expect(sent).toEqual([['PONG', {targetOrigin: 'https://example.com'}]]);
    expect(
      handleSharedTraceListPostMessage(event(opener, {traceUuids: ['u1']})),
    ).toEqual({traceUuids: ['u1']});
    expect(
      handleSharedTraceListPostMessage(event({}, {traceUuids: ['u1']})),
    ).toBeUndefined();
    vi.unstubAllGlobals();
  });
});
