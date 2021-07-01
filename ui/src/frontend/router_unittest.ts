// Copyright (C) 2021 The Android Open Source Project
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

import * as m from 'mithril';

import {NullAnalytics} from './analytics';
import {PageAttrs} from './pages';
import {Router} from './router';

const defaultMockComponent = {
  view() {}
};

const recordMockComponent = {
  view() {}
};

const fakeDispatch = () => {};
const mockLogging = new NullAnalytics();
const defaultRoute = '/';
const routes = new Map<string, m.Component<PageAttrs>>();
routes.set(defaultRoute, defaultMockComponent);
routes.set('/record', recordMockComponent);
const router = new Router(defaultRoute, routes, fakeDispatch, mockLogging);

test('empty route broken into empty components', () => {
  const {pageName, subpageName, component} = router['resolveOrDefault']('');
  expect(pageName).toBe(defaultRoute);
  expect(subpageName).toBe('');
  expect(component).toBe(defaultMockComponent);
});

test('invalid route broken into empty components', () => {
  const {pageName, subpageName, component} = router['resolveOrDefault']('bla');
  expect(pageName).toBe(defaultRoute);
  expect(subpageName).toBe('');
  expect(component).toBe(defaultMockComponent);
});

test('simple route has page defined', () => {
  const {pageName, subpageName, component} =
      router['resolveOrDefault']('/record');
  expect(pageName).toBe('/record');
  expect(subpageName).toBe('');
  expect(component).toBe(recordMockComponent);
});

test('simple route has both components defined', () => {
  const {pageName, subpageName, component} =
      router['resolveOrDefault']('/record/memory');
  expect(pageName).toBe('/record');
  expect(subpageName).toBe('/memory');
  expect(component).toBe(recordMockComponent);
});

test('route broken at first slash', () => {
  const {pageName, subpageName, component} =
      router['resolveOrDefault']('/record/memory/otherstuff');
  expect(pageName).toBe('/record');
  expect(subpageName).toBe('/memory/otherstuff');
  expect(component).toBe(recordMockComponent);
});

test('parameters separated from route', () => {
  const {pageName, subpageName, component, urlParams} =
      router['resolveOrDefault'](
          '/record/memory?url=http://localhost:1234/aaaa');
  expect(pageName).toBe('/record');
  expect(subpageName).toBe('/memory');
  expect(urlParams).toBe('?url=http://localhost:1234/aaaa');
  expect(component).toBe(recordMockComponent);
});
