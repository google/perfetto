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

import markdownit from 'markdown-it';
import {EvidenceStore, parseEvidence} from './evidence';
import {installEvidenceChips} from './markdown';

test('parseEvidence', () => {
  expect(parseEvidence('Q3')).toEqual({kind: 'query', id: 'Q3'});
  expect(parseEvidence('slice 1234')).toEqual({
    kind: 'event',
    table: 'slice',
    id: 1234,
  });
  expect(parseEvidence(' range  10 20 ')).toEqual({
    kind: 'range',
    start: 10n,
    end: 20n,
  });
  expect(parseEvidence('not evidence')).toBeUndefined();
  expect(parseEvidence('q3')).toBeUndefined();
  expect(parseEvidence('range 20 10')).toBeUndefined();
});

test('EvidenceStore assigns sequential query ids', () => {
  const store = new EvidenceStore();
  expect(store.addQuery('SELECT 1')).toBe('Q1');
  expect(store.addQuery('SELECT 2')).toBe('Q2');
  expect(store.getQuery('Q2')).toBe('SELECT 2');
  expect(store.getQuery('Q9')).toBeUndefined();
});

test('markdown renders citations as chips', () => {
  const md = installEvidenceChips(markdownit({html: false}).disable('image'));
  const html = md.renderInline('Blocked 182 ms [[Q2]] in [[slice 7]].');
  expect(html).toContain('data-evidence="Q2"');
  expect(html).toContain('data-evidence="slice 7"');
  expect(html).toContain('>Q2</button>');
  // Malformed citations and ordinary links are left alone.
  expect(md.renderInline('[[nope nope nope]]')).not.toContain('<button');
  expect(md.renderInline('[x](https://a.b)')).toContain('<a href');
});
