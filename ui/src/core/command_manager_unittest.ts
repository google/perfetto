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

import {CommandManagerImpl} from './command_manager';

const TestCommand = (id: string) => ({
  id,
  name: `Command ${id}`,
  callback: jest.fn(),
});

describe('CommandManagerImpl child manager', () => {
  test('child registry sees parent commands but not vice versa', () => {
    const parent = new CommandManagerImpl();
    const cmdParent = TestCommand('parent');
    parent.registerCommand(cmdParent);

    const child = parent.createChild();
    // Child sees parent's command
    expect(child.hasCommand('parent')).toBe(true);
    expect(child.getCommand('parent')).toBe(cmdParent);

    // Parent does NOT see child's command
    const cmdChild = TestCommand('child');
    child.registerCommand(cmdChild);
    expect(child.hasCommand('child')).toBe(true);
    expect(parent.hasCommand('child')).toBe(false);
  });
});
