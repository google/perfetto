// Copyright (C) 2025 The Android Open Source Project
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

import {fnvHash} from './graph_utils';

describe('fnvHash', () => {
  it('returns an 8-char hex string', () => {
    const h = fnvHash('hello');
    expect(h.length).toBe(8);
    expect(/^[0-9a-f]{8}$/.test(h)).toBe(true);
  });

  it('produces different hashes for different inputs', () => {
    expect(fnvHash('hello')).not.toBe(fnvHash('world'));
  });

  it('produces the same hash for the same input', () => {
    expect(fnvHash('test')).toBe(fnvHash('test'));
  });
});
