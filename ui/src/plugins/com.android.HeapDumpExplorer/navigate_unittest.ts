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

import {Time} from '../../base/time';
import {
  type NavLink,
  generateNavLink,
  generateNavSubpage,
  parseNavLink,
} from './navigate';

describe('navigate', () => {
  const dump = {upid: 42, ts: Time.fromRaw(1000n)};

  test('round-trips all tabs without dump', () => {
    const cases: Array<{link: NavLink; expectedHref: string}> = [
      {link: {tab: 'overview'}, expectedHref: '#!/heapdump/overview'},
      {link: {tab: 'flamegraph'}, expectedHref: '#!/heapdump/flamegraph'},
      {link: {tab: 'classes'}, expectedHref: '#!/heapdump/classes'},
      {
        link: {tab: 'classes', rootClass: 'java.lang.Object'},
        expectedHref: '#!/heapdump/classes?root=java.lang.Object',
      },
      {link: {tab: 'objects'}, expectedHref: '#!/heapdump/objects'},
      {
        link: {tab: 'objects', cls: 'android.graphics.Bitmap'},
        expectedHref: '#!/heapdump/objects_android.graphics.Bitmap',
      },
      {link: {tab: 'dominators'}, expectedHref: '#!/heapdump/dominators'},
      {link: {tab: 'bitmaps'}, expectedHref: '#!/heapdump/bitmaps'},
      {
        link: {tab: 'bitmaps', id: 0x1a, filterKey: 'foo'},
        expectedHref: '#!/heapdump/bitmaps_0x1a?fk=foo',
      },
      {link: {tab: 'strings'}, expectedHref: '#!/heapdump/strings'},
      {
        link: {tab: 'strings', q: 'hello world'},
        expectedHref: '#!/heapdump/strings?q=hello%20world',
      },
      {link: {tab: 'arrays'}, expectedHref: '#!/heapdump/arrays'},
      {
        link: {tab: 'arrays', arrayHash: 'abc'},
        expectedHref: '#!/heapdump/arrays?ah=abc',
      },
      {link: {tab: 'callstack'}, expectedHref: '#!/heapdump/callstack'},
      {
        link: {tab: 'object', id: 0x1234},
        expectedHref: '#!/heapdump/object_0x1234',
      },
      {
        link: {tab: 'flamegraph-objects'},
        expectedHref: '#!/heapdump/flamegraph_objects',
      },
      {
        link: {
          tab: 'flamegraph-objects',
          pathHashes: '10,20',
          isDominator: true,
        },
        expectedHref: '#!/heapdump/flamegraph_objects_1_10%2C20',
      },
    ];

    for (const {link, expectedHref} of cases) {
      expect(generateNavLink(link)).toBe(expectedHref);
      expect(parseNavLink(expectedHref)).toEqual(link);
    }
  });

  test('round-trips tabs with dump route prefix', () => {
    const link: NavLink = {
      tab: 'objects',
      cls: 'java.lang.String',
      dump,
    };
    const href = generateNavLink(link);
    expect(href).toBe('#!/heapdump/42-1000/objects_java.lang.String');
    expect(generateNavSubpage(link)).toBe('42-1000/objects_java.lang.String');
    expect(parseNavLink(href)).toEqual(link);
    expect(parseNavLink('42-1000/objects_java.lang.String')).toEqual(link);
  });

  test('parses bare dump route with default tab', () => {
    expect(parseNavLink('#!/heapdump/42-1000')).toEqual({
      tab: 'overview',
      dump,
    });
    expect(parseNavLink('42-1000', 'flamegraph')).toEqual({
      tab: 'flamegraph',
      dump,
    });
  });
});
