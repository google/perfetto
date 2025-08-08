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

import {parseAndSplitParams} from './plugin_manager';


test("Basic splitting", () => {
  const pluginParams = 'plugin-1:"something", plugin-2:"something"'

  expect(parseAndSplitParams(pluginParams)).toEqual(['plugin-1:"something"', 'plugin-2:"something"']);
});

test("Basic splitting without quotes", () => {
  const pluginParams = 'plugin-1:something, plugin-2:something'

  expect(parseAndSplitParams(pluginParams)).toEqual(['plugin-1:something', 'plugin-2:something']);
});

test("Comma inside quotes", () => {
  const pluginParams = 'plugin-1:"some,thing", plugin-2:"other"'

  expect(parseAndSplitParams(pluginParams)).toEqual(['plugin-1:"some,thing"', 'plugin-2:"other"']);
});

test("Comma inside for both quotes", () => {
  const pluginParams = 'plugin-1:"some,thing", plugin-2:"other, thing"'

  expect(parseAndSplitParams(pluginParams)).toEqual(['plugin-1:"some,thing"', 'plugin-2:"other, thing"']);
});

test("Empty pluginParams", () => {
  const pluginParams = ''

  expect(parseAndSplitParams(pluginParams)).toEqual([]);
});
