// Copyright (C) 2018 The Android Open Source Project
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

import {Router} from './router';

const mockComponent = {
  view() {},
};

beforeEach(() => {
  window.location.hash = '';
});

test('Default route must be defined', () => {
  expect(() => new Router({'/a': mockComponent})).toThrow();
});

test('Resolves empty route to default component', () => {
  const router = new Router({'/': mockComponent});
  window.location.hash = '';
  expect(router.resolve().tag).toBe(mockComponent);
});

test('Resolves subpage route to component of main page', () => {
  const nonDefaultComponent = {view() {}};
  const router = new Router({
    '/': mockComponent,
    '/a': nonDefaultComponent,
  });
  window.location.hash = '#!/a/subpage';
  expect(router.resolve().tag).toBe(nonDefaultComponent);
  expect(router.resolve().attrs.subpage).toBe('/subpage');
});

test('Pass empty subpage if not found in URL', () => {
  const nonDefaultComponent = {view() {}};
  const router = new Router({
    '/': mockComponent,
    '/a': nonDefaultComponent,
  });
  window.location.hash = '#!/a';
  expect(router.resolve().tag).toBe(nonDefaultComponent);
  expect(router.resolve().attrs.subpage).toBe('');
});

test('Args parsing', () => {
  const url = 'http://localhost/#!/foo?p=123&s=42&url=a?b?c';
  const args = Router.parseUrl(url).args;
  expect(args.p).toBe('123');
  expect(args.s).toBe('42');
  expect(args.url).toBe('a?b?c');
});

test('empty route broken into empty components', () => {
  const {page, subpage, args} = Router.parseFragment('');
  expect(page).toBe('');
  expect(subpage).toBe('');
  expect(args).toEqual({});
});

test('invalid route broken into empty components', () => {
  const {page, subpage, args} = Router.parseFragment('/bla');
  expect(page).toBe('');
  expect(subpage).toBe('');
  expect(args).toEqual({});
});

test('simple route has page defined', () => {
  const {page, subpage, args} = Router.parseFragment('#!/record');
  expect(page).toBe('/record');
  expect(subpage).toBe('');
  expect(args).toEqual({});
});

test('simple route has both components defined', () => {
  const {page, subpage, args} = Router.parseFragment('#!/record/memory');
  expect(page).toBe('/record');
  expect(subpage).toBe('/memory');
  expect(args).toEqual({});
});

test('route broken at first slash', () => {
  const {page, subpage, args} = Router.parseFragment('#!/record/memory/stuff');
  expect(page).toBe('/record');
  expect(subpage).toBe('/memory/stuff');
  expect(args).toEqual({});
});

test('parameters separated from route', () => {
  const {page, subpage, args} =
      Router.parseFragment('#!/record/memory?url=http://localhost:1234/aaaa');
  expect(page).toBe('/record');
  expect(subpage).toBe('/memory');
  expect(args).toEqual({url: 'http://localhost:1234/aaaa'});
});
