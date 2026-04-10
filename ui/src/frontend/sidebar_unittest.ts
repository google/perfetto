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

import {pageMatchesHref} from './sidebar';

describe('pageMatchesHref', () => {
  test('direct match', () => {
    window.location.hash = '#!/record';
    expect(pageMatchesHref('#!/record')).toBe(true);
  });

  test('with subpage', () => {
    window.location.hash = '#!/record/memory';
    expect(pageMatchesHref('#!/record')).toBe(true);
  });

  test('different page', () => {
    window.location.hash = '#!/timeline';
    expect(pageMatchesHref('#!/record')).toBe(false);
  });

  test('homepage', () => {
    window.location.hash = '';
    expect(pageMatchesHref('#!/record')).toBe(false);
  });

  test('homepage with shebang', () => {
    window.location.hash = '#!/';
    expect(pageMatchesHref('#!/record')).toBe(false);
  });

  test('partial match', () => {
    window.location.hash = '#!/rec';
    expect(pageMatchesHref('#!/record')).toBe(false);
  });

  test('partial match reverse', () => {
    window.location.hash = '#!/record';
    expect(pageMatchesHref('#!/rec')).toBe(false);
  });

  test('external links', () => {
    window.location.hash = '';
    expect(pageMatchesHref('https://example.com/')).toBe(false);
    expect(pageMatchesHref('https://example.com/#!/')).toBe(false);
  });

  test('external link with same fragment', () => {
    window.location.hash = '#!/record';
    expect(pageMatchesHref('https://example.com/#!/record')).toBe(false);
  });
});
