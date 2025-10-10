// Copyright (C) 2024 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {PageManagerImpl} from './page_manager';

const TestPage = (route: string) => ({
  route,
  pluginId: 'testPlugin',
  render: jest.fn(),
});

describe('PageManagerImpl child manager', () => {
  test('child registry sees parent pages but not vice versa', () => {
    const parent = new PageManagerImpl();
    const pageParent = TestPage('/parent');
    parent.registerPage(pageParent);

    const child = parent.createChild();
    // Child sees parent's page
    child['renderPageForRoute']('/parent', '');
    expect(pageParent.render).toHaveBeenCalled();

    // Parent does NOT see child's page
    const pageChild = TestPage('/child');
    child.registerPage(pageChild);
    parent['renderPageForRoute']('/child', '');
    expect(pageChild.render).not.toHaveBeenCalled();
  });
});
