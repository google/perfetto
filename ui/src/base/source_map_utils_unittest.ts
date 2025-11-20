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

import {decodeVLQ, findOriginalPosition, SourceMap} from './source_map_utils';

// This is a new test file that should be created.
describe('source_map_utils', () => {
  describe('decodeVLQ', () => {
    it('should decode zero', () => {
      expect(decodeVLQ('A')).toEqual([0]);
    });

    it('should decode positive numbers', () => {
      expect(decodeVLQ('C')).toEqual([1]);
      expect(decodeVLQ('E')).toEqual([2]);
      expect(decodeVLQ('G')).toEqual([3]);
    });

    it('should decode negative numbers', () => {
      expect(decodeVLQ('D')).toEqual([-1]);
      expect(decodeVLQ('F')).toEqual([-2]);
      expect(decodeVLQ('H')).toEqual([-3]);
    });

    it('should decode multi-character values', () => {
      expect(decodeVLQ('kC')).toEqual([34]);
    });

    it('should decode multiple values', () => {
      expect(decodeVLQ('AAAA')).toEqual([0, 0, 0, 0]);
      expect(decodeVLQ('AACE')).toEqual([0, 0, 1, 2]);
    });

    it('should decode realistic source map segment', () => {
      expect(decodeVLQ('AAAA')).toEqual([0, 0, 0, 0]);
    });
  });

  describe('findOriginalPosition', () => {
    const testSourceMap: SourceMap = {
      version: 3,
      sources: ['src/foo.ts', 'src/bar.ts'],
      names: ['myFunction', 'myVar'],
      mappings: 'AAAA,UAAKA;',
    };

    it('should map position at exact column match', () => {
      const pos = findOriginalPosition(testSourceMap, 1, 0);
      expect(pos.source).toBe('src/foo.ts');
      expect(pos.line).toBe(1);
      expect(pos.column).toBe(0);
    });

    it('should map position to closest preceding mapping', () => {
      const pos = findOriginalPosition(testSourceMap, 1, 5);
      expect(pos.source).toBe('src/foo.ts');
      expect(pos.line).toBe(1);
      expect(pos.column).toBe(0);
    });

    it('should return null for line out of range', () => {
      const pos = findOriginalPosition(testSourceMap, 999, 0);
      expect(pos.source).toBeNull();
      expect(pos.line).toBeNull();
      expect(pos.column).toBeNull();
    });

    it('should handle empty line', () => {
      const mapWithEmptyLine: SourceMap = {
        version: 3,
        sources: ['test.ts'],
        names: [],
        mappings: ';AAAA',
      };
      const pos = findOriginalPosition(mapWithEmptyLine, 1, 0);
      expect(pos.source).toBeNull();
    });

    it('should handle column beyond all mappings', () => {
      const pos = findOriginalPosition(testSourceMap, 1, 1000);
      expect(pos.source).not.toBeNull();
    });
  });

  describe('findOriginalPosition with multiple sources', () => {
    const multiSourceMap: SourceMap = {
      version: 3,
      sources: ['file1.ts', 'file2.ts', 'file3.ts'],
      names: [],
      mappings: 'AAAA;ACAA;ACAA',
    };

    it('should map to first source file', () => {
      const pos = findOriginalPosition(multiSourceMap, 1, 0);
      expect(pos.source).toBe('file1.ts');
    });

    it('should map to second source file', () => {
      const pos = findOriginalPosition(multiSourceMap, 2, 0);
      expect(pos.source).toBe('file2.ts');
    });

    it('should map to third source file', () => {
      const pos = findOriginalPosition(multiSourceMap, 3, 0);
      expect(pos.source).toBe('file3.ts');
    });
  });

  describe('findOriginalPosition with negative deltas', () => {
    const deltaSourceMap: SourceMap = {
      version: 3,
      sources: ['a.ts', 'b.ts', 'c.ts'],
      names: [],
      mappings: 'AAAA,CCAA,EDAA',
    };

    it('should handle negative source index deltas', () => {
      const pos1 = findOriginalPosition(deltaSourceMap, 1, 0);
      expect(pos1.source).toBe('a.ts');

      const pos2 = findOriginalPosition(deltaSourceMap, 1, 2);
      expect(pos2.source).toBe('b.ts');

      const pos3 = findOriginalPosition(deltaSourceMap, 1, 4);
      expect(pos3.source).toBe('a.ts');
    });
  });

  describe('findOriginalPosition with multi-line source map', () => {
    const multiLineSourceMap: SourceMap = {
      version: 3,
      sources: ['original.ts'],
      names: ['func1', 'func2', 'func3'],
      mappings: ['AAAAA', 'AAAAC', '', 'AAAAC'].join(';'),
    };

    it('should map different lines correctly', () => {
      const pos1 = findOriginalPosition(multiLineSourceMap, 1, 0);
      expect(pos1.source).toBe('original.ts');
      expect(pos1.name).toBe('func1');

      const pos2 = findOriginalPosition(multiLineSourceMap, 2, 0);
      expect(pos2.source).toBe('original.ts');
      expect(pos2.name).toBe('func2');

      const pos3 = findOriginalPosition(multiLineSourceMap, 3, 0);
      expect(pos3.source).toBeNull();

      const pos4 = findOriginalPosition(multiLineSourceMap, 4, 0);
      expect(pos4.source).toBe('original.ts');
      expect(pos4.name).toBe('func3');
    });
  });

  describe('edge cases', () => {
    it('should handle source map with no sources', () => {
      const emptySourceMap: SourceMap = {
        version: 3,
        sources: [],
        names: [],
        mappings: '',
      };
      const pos = findOriginalPosition(emptySourceMap, 1, 0);
      expect(pos.source).toBeNull();
    });

    it('should handle invalid segment data gracefully', () => {
      const invalidSourceMap: SourceMap = {
        version: 3,
        sources: ['test.ts'],
        names: [],
        mappings: 'A',
      };
      const pos = findOriginalPosition(invalidSourceMap, 1, 0);
      expect(pos.source).toBeNull();
    });
  });
});
