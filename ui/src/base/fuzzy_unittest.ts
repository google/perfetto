// Copyright (C) 2023 The Android Open Source Project
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

import {fuzzySearch} from './fuzzy';

describe('fuzzySearch', () => {
  describe('check relevant results are matched', () => {
    function chkFuzz(
      items: readonly string[],
      searchTerm: string,
      expectedToMatch: readonly string[],
    ) {
      const results = fuzzySearch(items, (x) => x, searchTerm).map(
        (r) => r.item,
      );
      expect(results).toEqual(expect.arrayContaining([...expectedToMatch]));
      expect(results.length).toBe(expectedToMatch.length);
    }

    it('matches uris', () => {
      const plugins = [
        'dev.perfetto.RecordTraceV2',
        'com.android.XMLParser',
        'com.meta.GpuCompute',
      ];

      chkFuzz(plugins, 'gpucompute', ['com.meta.GpuCompute']);
      chkFuzz(plugins, 'gpu', ['com.meta.GpuCompute']);
      chkFuzz(plugins, 'compute', ['com.meta.GpuCompute']);
      chkFuzz(plugins, 'gpu compute', ['com.meta.GpuCompute']);
      chkFuzz(plugins, 'compute gpu', ['com.meta.GpuCompute']);
      chkFuzz(plugins, 'com meta gpucompute', ['com.meta.GpuCompute']);
      chkFuzz(plugins, 'GpuCompute', ['com.meta.GpuCompute']);
      chkFuzz(plugins, 'com.meta.GpuCompute', ['com.meta.GpuCompute']);

      chkFuzz(plugins, 'record', ['dev.perfetto.RecordTraceV2']);
      chkFuzz(plugins, 'v2', ['dev.perfetto.RecordTraceV2']);
      chkFuzz(plugins, 'perfetto recordtrace', ['dev.perfetto.RecordTraceV2']);
      chkFuzz(plugins, 'perfetto record trace', ['dev.perfetto.RecordTraceV2']);
      chkFuzz(plugins, 'trace record', ['dev.perfetto.RecordTraceV2']);
      chkFuzz(plugins, 'dev perfetto record trace v2', [
        'dev.perfetto.RecordTraceV2',
      ]);
      chkFuzz(plugins, 'dev.perfetto.RecordTraceV2', [
        'dev.perfetto.RecordTraceV2',
      ]);

      chkFuzz(plugins, 'xml parser', ['com.android.XMLParser']);
      chkFuzz(plugins, 'com android xml parser', ['com.android.XMLParser']);
      chkFuzz(plugins, 'com.android.XMLParser', ['com.android.XMLParser']);
    });
  });

  describe('sort order ranking', () => {
    it('ranks exact and substring matches higher than loose fuzzy matches', () => {
      const items = [
        't_r_a_c_e_scattered',
        'trace_processor',
        'trace',
        'record_trace',
      ];
      const results = fuzzySearch(items, (x) => x, 'trace').map((r) => r.item);
      expect(results[0]).toBe('trace');
      expect(results.indexOf('trace_processor')).toBeLessThan(
        results.indexOf('t_r_a_c_e_scattered'),
      );
    });

    it('ranks exact match above partial matches', () => {
      const items = ['foobar', 'foo', 'barfoo'];
      const results = fuzzySearch(items, (x) => x, 'foo').map((r) => r.item);
      expect(results[0]).toBe('foo');
    });
  });

  describe('score ordering', () => {
    it('returns results sorted by score in non-increasing order', () => {
      const items = [
        'trace',
        'trace_processor',
        'record_trace',
        're_trace_action',
        't_r_a_c_e',
      ];
      const results = fuzzySearch(items, (x) => x, 'trace');
      for (let i = 0; i < results.length - 1; i++) {
        expect(results[i].score).toBeGreaterThanOrEqual(results[i + 1].score);
      }
    });

    it('returns uniform score of 1 for empty search term in original item order', () => {
      const items = ['c', 'a', 'b'];
      const results = fuzzySearch(items, (x) => x, '');
      expect(results.map((r) => r.item)).toEqual(['c', 'a', 'b']);
      expect(results.every((r) => r.score === 1)).toBe(true);
    });
  });

  describe('highlight segments integrity and accuracy', () => {
    it('reconstructs original text when joining segment values', () => {
      const items = [
        'dev.perfetto.LiveMemory',
        'gpu_compute_task',
        'exact_match',
      ];
      const results = fuzzySearch(items, (x) => x, 'memory');
      for (const res of results) {
        const reconstructed = res.segments.map((s) => s.value).join('');
        expect(reconstructed).toBe(res.item);
      }
    });

    it('produces valid matching segments for exact match', () => {
      const results = fuzzySearch(['foo'], (x) => x, 'foo');
      expect(results[0].segments).toEqual([{matching: true, value: 'foo'}]);
    });

    it('produces non-matching single segment for empty search term', () => {
      const results = fuzzySearch(['foo'], (x) => x, '');
      expect(results[0].segments).toEqual([{matching: false, value: 'foo'}]);
    });

    it('correctly splits matching and non-matching segments', () => {
      const results = fuzzySearch(['ababc'], (x) => x, 'abc');
      expect(results.length).toBe(1);
      const reconstructed = results[0].segments.map((s) => s.value).join('');
      expect(reconstructed).toBe('ababc');
      const matchedText = results[0].segments
        .filter((s) => s.matching)
        .map((s) => s.value)
        .join('');
      expect(matchedText).toBe('abc');
    });
  });

  describe('multi-key lookup', () => {
    interface CommandItem {
      name: string;
      source: string;
    }

    const items: CommandItem[] = [
      {name: 'Table List', source: 'DataExplorer'},
      {name: 'Record Trace', source: 'Core'},
      {name: 'Data Engine', source: 'BigTrace'},
    ];

    const keyLookups = [
      (x: CommandItem) => x.name,
      (x: CommandItem) => x.source,
    ];

    it('matches on primary or secondary keys', () => {
      const nameResults = fuzzySearch(items, keyLookups, 'Table');
      expect(nameResults.map((r) => r.item.name)).toEqual(['Table List']);

      const sourceResults = fuzzySearch(items, keyLookups, 'BigTrace');
      expect(sourceResults.map((r) => r.item.name)).toEqual(['Data Engine']);
    });

    it('returns per-key segments that reconstruct original key texts', () => {
      const results = fuzzySearch(items, keyLookups, 'Data');
      expect(results.length).toBeGreaterThan(0);
      for (const res of results) {
        expect(res.segments[0].map((s) => s.value).join('')).toBe(
          res.item.name,
        );
        expect(res.segments[1].map((s) => s.value).join('')).toBe(
          res.item.source,
        );
      }
    });

    it('handles empty search query for multi-key lookup', () => {
      const results = fuzzySearch(items, keyLookups, '');
      expect(results.length).toBe(items.length);
      for (let i = 0; i < items.length; i++) {
        expect(results[i].item).toEqual(items[i]);
        expect(results[i].score).toBe(1);
        expect(results[i].segments[0]).toEqual([
          {matching: false, value: items[i].name},
        ]);
        expect(results[i].segments[1]).toEqual([
          {matching: false, value: items[i].source},
        ]);
      }
    });

    it('orders multi-key results by overall match score', () => {
      const results = fuzzySearch(items, keyLookups, 'Data');
      for (let i = 0; i < results.length - 1; i++) {
        expect(results[i].score).toBeGreaterThanOrEqual(results[i + 1].score);
      }
    });
  });
});
