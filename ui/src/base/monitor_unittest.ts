// Copyright (C) 2024 The Android Open Source Project
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

import {Monitor} from './monitor';

test('callback is called when state changes', () => {
  const reducer = jest.fn().mockReturnValue('foo');
  const monitor = new Monitor([reducer]);
  const mockCallback = jest.fn();

  monitor.ifStateChanged(mockCallback);
  expect(mockCallback).toHaveBeenCalledTimes(1);

  mockCallback.mockReset();
  monitor.ifStateChanged(mockCallback);
  monitor.ifStateChanged(mockCallback);
  expect(mockCallback).not.toHaveBeenCalled();

  mockCallback.mockReset();
  reducer.mockReturnValue('bar');
  monitor.ifStateChanged(mockCallback);
  expect(mockCallback).toHaveBeenCalledTimes(1);

  mockCallback.mockReset();
  monitor.ifStateChanged(mockCallback);
  expect(mockCallback).not.toHaveBeenCalled();
});
