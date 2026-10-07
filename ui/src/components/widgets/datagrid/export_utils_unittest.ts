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

import {defaultValueFormatter} from './export_utils';

describe('export_utils', () => {
  describe('defaultValueFormatter', () => {
    it('formats null values', () => {
      expect(defaultValueFormatter(null)).toEqual('null');
    });

    it('formats string values', () => {
      expect(defaultValueFormatter('hello')).toEqual('hello');
    });

    it('formats number values', () => {
      expect(defaultValueFormatter(123)).toEqual('123');
      expect(defaultValueFormatter(123.456)).toEqual('123.456');
    });

    it('formats bigint values', () => {
      expect(defaultValueFormatter(123n)).toEqual('123');
    });

    it('formats Uint8Array values', () => {
      const blob = new Uint8Array([1, 2, 3, 4, 5]);
      expect(defaultValueFormatter(blob)).toEqual('Blob: 5 bytes');
    });
  });
});
