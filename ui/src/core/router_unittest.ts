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

describe('Router#resolve', () => {
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
});

describe('Router.parseUrl', () => {
  // Can parse arguments from the search string.
  test('Search parsing', () => {
    const url = 'http://localhost?p=123&s=42&url=a?b?c';
    const route = Router.parseUrl(url);
    const args = route.args;
    expect(args.p).toBe('123');
    expect(args.s).toBe('42');
    expect(args.url).toBe('a?b?c');
    expect(route.fragment).toBe('');
  });

  // Or from the fragment string.
  test('Fragment parsing', () => {
    const url = 'http://localhost/#!/foo?p=123&s=42&url=a?b?c';
    const route = Router.parseUrl(url);
    const args = route.args;
    expect(args.p).toBe('123');
    expect(args.s).toBe('42');
    expect(args.url).toBe('a?b?c');
    expect(route.fragment).toBe('');
  });

  // Or both in which case fragment overrides the search.
  test('Fragment parsing', () => {
    const url =
      'http://localhost/?p=1&s=2&hideSidebar=true#!/foo?s=3&url=4&hideSidebar=false';
    const route = Router.parseUrl(url);
    const args = route.args;
    expect(args.p).toBe('1');
    expect(args.s).toBe('3');
    expect(args.url).toBe('4');
    expect(args.hideSidebar).toBe(false);
    expect(route.fragment).toBe('');
  });

  // + is also space
  test('plus is space query', () => {
    const url = 'http://localhost?query=(foo+%2B+bar),';
    const route = Router.parseUrl(url);
    const args = route.args;
    expect(args.query).toBe('(foo + bar),');
  });

  // + is also space
  test('plus is space hash', () => {
    const url = 'http://localhost#!/foo?query=(foo+%2B+bar),';
    const route = Router.parseUrl(url);
    const args = route.args;
    expect(args.query).toBe('(foo + bar),');
  });

  test('Nested fragment', () => {
    const url =
      'http://localhost/?p=1&s=2&hideSidebar=true#!/foo?s=3&url=4&hideSidebar=false#myfragment';
    const route = Router.parseUrl(url);
    expect(route.fragment).toBe('myfragment');
  });
});

describe('Router.parseFragment', () => {
  test('empty route broken into empty components', () => {
    const {page, subpage, args} = Router.parseFragment('');
    expect(page).toBe('');
    expect(subpage).toBe('');
    expect(args.mode).toBe(undefined);
  });

  test('by default args are undefined', () => {
    // This prevents the url from becoming messy.
    const {args} = Router.parseFragment('');
    expect(args).toEqual({});
  });

  test('invalid route broken into empty components', () => {
    const {page, subpage} = Router.parseFragment('/bla');
    expect(page).toBe('');
    expect(subpage).toBe('');
  });

  test('simple route has page defined', () => {
    const {page, subpage} = Router.parseFragment('#!/record');
    expect(page).toBe('/record');
    expect(subpage).toBe('');
  });

  test('simple route has both components defined', () => {
    const {page, subpage} = Router.parseFragment('#!/record/memory');
    expect(page).toBe('/record');
    expect(subpage).toBe('/memory');
  });

  test('route broken at first slash', () => {
    const {page, subpage} = Router.parseFragment('#!/record/memory/stuff');
    expect(page).toBe('/record');
    expect(subpage).toBe('/memory/stuff');
  });

  test('parameters separated from route', () => {
    const {page, subpage, args} = Router.parseFragment(
      '#!/record/memory?url=http://localhost:1234/aaaa',
    );
    expect(page).toBe('/record');
    expect(subpage).toBe('/memory');
    expect(args.url).toEqual('http://localhost:1234/aaaa');
  });

  test('openFromAndroidBugTool can be false', () => {
    const {args} = Router.parseFragment('#!/?openFromAndroidBugTool=false');
    expect(args.openFromAndroidBugTool).toEqual(false);
  });

  test('openFromAndroidBugTool can be true', () => {
    const {args} = Router.parseFragment('#!/?openFromAndroidBugTool=true');
    expect(args.openFromAndroidBugTool).toEqual(true);
  });

  test('bad modes are coerced to default', () => {
    const {args} = Router.parseFragment('#!/?mode=1234');
    expect(args.mode).toEqual(undefined);
  });

  test('bad hideSidebar is coerced to default', () => {
    const {args} = Router.parseFragment('#!/?hideSidebar=helloworld!');
    expect(args.hideSidebar).toEqual(undefined);
  });
});
