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

import {dingus} from 'dingusjs';
import * as m from 'mithril';

import {Actions, DeferredAction} from '../common/actions';

import {NullAnalytics} from './analytics';
import {PageAttrs} from './pages';
import {Router} from './router';

const mockComponent = {
  view() {}
};

const fakeDispatch = () => {};

const mockLogging = new NullAnalytics();

beforeEach(() => {
  window.onhashchange = null;
  window.location.hash = '';
});

test('Default route must be defined', () => {
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/b', mockComponent);
  expect(() => new Router('/a', routes, fakeDispatch, mockLogging)).toThrow();
});

test('Resolves empty route to default component', () => {
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/a', mockComponent);
  const router = new Router('/a', routes, fakeDispatch, mockLogging);
  expect(router.resolve('').tag).toBe(mockComponent);
  expect(router.resolve(undefined).tag).toBe(mockComponent);
});

test('Resolves subpage route to component of main page', () => {
  const nonDefaultComponent = {view() {}};
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  routes.set('/a', nonDefaultComponent);
  const router = new Router('/', routes, fakeDispatch, mockLogging);
  expect(router.resolve('/a/subpage').tag).toBe(nonDefaultComponent);
});

test('Parse route from hash', () => {
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  const router = new Router('/', routes, fakeDispatch, mockLogging);
  window.location.hash = '#!/foobar?s=42';
  expect(router.getFullRouteFromHash()).toBe('/foobar?s=42');

  window.location.hash = '/foobar';  // Invalid prefix.
  expect(router.getFullRouteFromHash()).toBe('');
});

test('Set valid route on hash', () => {
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  routes.set('/a', mockComponent);
  const dispatch = dingus<(a: DeferredAction) => void>();
  const router = new Router('/', routes, dispatch, mockLogging);
  const prevHistoryLength = window.history.length;

  router.setRouteOnHash('/a');
  expect(window.location.hash).toBe('#!/a');
  expect(window.history.length).toBe(prevHistoryLength + 1);
  // No navigation action should be dispatched.
  expect(dispatch.calls.length).toBe(0);
});

test('Set valid route with arguments on hash', () => {
  const dispatch = dingus<(a: DeferredAction) => void>();
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  routes.set('/a', mockComponent);
  const router = new Router('/', routes, dispatch, mockLogging);
  const prevHistoryLength = window.history.length;

  router.setRouteOnHash('/a', '?trace_id=aaa');
  expect(window.location.hash).toBe('#!/a?trace_id=aaa');
  expect(window.history.length).toBe(prevHistoryLength + 1);
  // No navigation action should be dispatched.
  expect(dispatch.calls.length).toBe(0);
});

test('Redirects to default for invalid route in setRouteOnHash ', () => {
  const dispatch = dingus<(a: DeferredAction) => void>();
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);

  const router = new Router('/', routes, dispatch, mockLogging);
  router.setRouteOnHash('foo');
  expect(dispatch.calls.length).toBe(1);
  expect(dispatch.calls[0][1].length).toBeGreaterThanOrEqual(1);
  expect(dispatch.calls[0][1][0]).toEqual(Actions.navigate({route: '/'}));
});

test('Navigate on hash change', done => {
  const mockDispatch = (a: DeferredAction) => {
    expect(a).toEqual(Actions.navigate({route: '/viewer'}));
    done();
  };
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  routes.set('/viewer', mockComponent);

  new Router('/', routes, mockDispatch, mockLogging);
  window.location.hash = '#!/viewer';
});

test('Redirects to default when invalid route set in window location', done => {
  const mockDispatch = (a: DeferredAction) => {
    expect(a).toEqual(Actions.navigate({route: '/'}));
    done();
  };
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  routes.set('/viewer', mockComponent);

  new Router('/', routes, mockDispatch, mockLogging);

  window.location.hash = '#invalid';
});

test('navigateToCurrentHash with valid current route', () => {
  const dispatch = dingus<(a: DeferredAction) => void>();
  window.location.hash = '#!/b';
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  routes.set('/b', mockComponent);
  const router = new Router('/', routes, dispatch, mockLogging);
  router.navigateToCurrentHash();
  expect(dispatch.calls[0][1][0]).toEqual(Actions.navigate({route: '/b'}));
});

test('navigateToCurrentHash with valid subpage', () => {
  const dispatch = dingus<(a: DeferredAction) => void>();
  window.location.hash = '#!/b/subpage';
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  routes.set('/b', mockComponent);
  const router = new Router('/', routes, dispatch, mockLogging);
  router.navigateToCurrentHash();
  expect(dispatch.calls[0][1][0]).toEqual(Actions.navigate({
    route: '/b/subpage'
  }));
});

test('navigateToCurrentHash with invalid current route', () => {
  const dispatch = dingus<(a: DeferredAction) => void>();
  window.location.hash = '#!/invalid';
  const routes = new Map<string, m.Component<PageAttrs>>();
  routes.set('/', mockComponent);
  const router = new Router('/', routes, dispatch, mockLogging);
  router.navigateToCurrentHash();
  expect(dispatch.calls[0][1][0]).toEqual(Actions.navigate({route: '/'}));
});

test('Params parsing', () => {
  window.location.hash = '#!/foo?a=123&b=42&c=a?b?c';
  expect(Router.param('a')).toBe('123');
  expect(Router.param('b')).toBe('42');
  expect(Router.param('c')).toBe('a?b?c');
});
