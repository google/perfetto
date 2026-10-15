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

import {HeaderManagerImpl} from './header_manager';

// Renders the items, e.g. ['a', 'b'].
function render(header: HeaderManagerImpl) {
  return Array.from(header.items.values(), (item) => item.render());
}

describe('HeaderManagerImpl', () => {
  test('lists items in registration order', () => {
    const header = new HeaderManagerImpl();
    header.registerItem({render: () => 'a'});
    header.registerItem({render: () => 'b'});

    expect(render(header)).toEqual(['a', 'b']);
  });

  test('removes an item when disposed', () => {
    const header = new HeaderManagerImpl();
    header.registerItem({render: () => 'a'});
    const b = header.registerItem({render: () => 'b'});
    header.registerItem({render: () => 'c'});

    b[Symbol.dispose]();
    expect(render(header)).toEqual(['a', 'c']);

    // Later items don't replace remaining ones.
    header.registerItem({render: () => 'd'});
    expect(render(header)).toEqual(['a', 'c', 'd']);
  });

  test('ignores a second dispose', () => {
    const header = new HeaderManagerImpl();
    const a = header.registerItem({render: () => 'a'});
    header.registerItem({render: () => 'b'});

    a[Symbol.dispose]();
    a[Symbol.dispose]();
    expect(render(header)).toEqual(['b']);
  });
});
