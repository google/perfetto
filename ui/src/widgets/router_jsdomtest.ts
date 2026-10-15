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

import m from 'mithril';
import {afterEach, beforeEach, describe, expect, test, vi} from 'vitest';
import {prettyDOM} from '@testing-library/dom';
import {
  type RouteHandler,
  Router,
  type RouterAttrs,
  type Routes,
} from './router';

// DOM tests for the Router widget. Router is rendered into `container` (jsdom,
// set globally in vitest.config.mjs) via m.render and the resulting DOM is
// queried. Failures attach a prettyDOM() dump as the assertion message.

let container: HTMLElement;

beforeEach(() => {
  container = document.createElement('div');
  document.body.appendChild(container);
});

afterEach(() => {
  m.render(container, null);
  container.remove();
});

function dumpDom(): string {
  const out = prettyDOM(container);
  return typeof out === 'string' ? out : '';
}

function render(attrs: RouterAttrs) {
  m.render(container, m(Router, attrs));
}

// Returns the text of the element with the given class, or undefined if it is
// not in the DOM.
function textOf(cls: string): string | undefined {
  const el = container.querySelector(`.${cls}`);
  return el?.textContent ?? undefined;
}

const fallback: RouteHandler = () => m('.fallback', 'Not found');

describe('Router', () => {
  const routes: Routes = {
    'overview': () => m('.overview', 'Overview'),
    'object/:id': ({params}) => m('.object', params.id),
    'a/:x/b/:y': ({params}) => m('.pair', `${params.x},${params.y}`),
  };

  describe('when matching a path', () => {
    test('renders the handler for a literal route', () => {
      render({path: 'overview', routes, fallback});
      expect(textOf('overview'), dumpDom()).toBe('Overview');
      expect(textOf('fallback'), dumpDom()).toBeUndefined();
    });

    test('ignores leading and trailing slashes', () => {
      render({path: '/overview/', routes, fallback});
      expect(textOf('overview'), dumpDom()).toBe('Overview');
    });

    test('renders the fallback on a segment-count mismatch', () => {
      render({path: '/object/1/extra', routes, fallback});
      expect(textOf('object'), dumpDom()).toBeUndefined();
      expect(textOf('fallback'), dumpDom()).toBe('Not found');
    });

    test('handles empty paths', () => {
      render({
        path: '',
        routes: {'': () => m('.root', 'Root'), ...routes},
        fallback,
      });
      expect(textOf('root'), dumpDom()).toBe('Root');
    });

    test('prefers the first matching route', () => {
      render({
        path: '/object/special',
        routes: {
          'object/special': () => m('.special', 'Special'),
          'object/:id': ({params}) => m('.object', params.id),
        },
        fallback,
      });
      expect(textOf('special'), dumpDom()).toBe('Special');
      expect(textOf('object'), dumpDom()).toBeUndefined();
    });

    test('calls only the matched handler', () => {
      const overview = vi.fn(() => m('.overview'));
      const object = vi.fn(() => m('.object'));
      const fb = vi.fn(() => m('.fallback'));
      render({
        path: '/object/1',
        routes: {'overview': overview, 'object/:id': object},
        fallback: fb,
      });
      expect(overview).not.toHaveBeenCalled();
      expect(object).toHaveBeenCalledTimes(1);
      expect(object).toHaveBeenCalledWith({params: {id: '1'}});
      expect(fb).not.toHaveBeenCalled();
    });
  });

  describe('with parameterised routes', () => {
    test('captures :param segments', () => {
      render({path: '/object/0x1a', routes, fallback});
      expect(textOf('object'), dumpDom()).toBe('0x1a');
    });

    test('captures multiple :param segments', () => {
      render({path: '/a/1/b/2', routes, fallback});
      expect(textOf('pair'), dumpDom()).toBe('1,2');
    });

    test('URI-decodes captured params', () => {
      render({path: '/object/foo%20bar%2Fbaz', routes, fallback});
      expect(textOf('object'), dumpDom()).toBe('foo bar/baz');
    });

    test('renders the fallback on malformed percent-encoding', () => {
      render({path: '/object/%E0%A4', routes, fallback});
      expect(textOf('object'), dumpDom()).toBeUndefined();
      expect(textOf('fallback'), dumpDom()).toBe('Not found');
    });
  });

  describe('when no route matches', () => {
    test('renders the fallback', () => {
      render({path: '/nope', routes, fallback});
      expect(textOf('fallback'), dumpDom()).toBe('Not found');
    });

    test('renders nothing if there is no fallback', () => {
      render({path: '/nope', routes});
      expect(container.childNodes.length, dumpDom()).toBe(0);
    });

    test('passes empty params to the fallback', () => {
      const fb = vi.fn(() => m('.fallback'));
      render({path: '/nope', routes, fallback: fb});
      expect(fb).toHaveBeenCalledWith({params: {}});
    });
  });

  describe('when uncached', () => {
    test('unmounts the previous route on navigation', () => {
      let inits = 0;
      let removes = 0;
      const Stateful: m.Component = {
        oninit: () => {
          inits++;
        },
        onremove: () => {
          removes++;
        },
        view: () => m('.stateful'),
      };
      const statefulRoutes: Routes = {
        stateful: () => m(Stateful),
        other: () => m('.other'),
      };

      render({path: '/stateful', routes: statefulRoutes});
      expect(inits).toBe(1);

      render({path: '/other', routes: statefulRoutes});
      expect(container.querySelector('.stateful'), dumpDom()).toBeNull();
      expect(removes).toBe(1);

      // Navigating back mounts a fresh instance.
      render({path: '/stateful', routes: statefulRoutes});
      expect(inits).toBe(2);
    });
  });

  describe('when cached', () => {
    // Returns the Gate wrapper element around the element with the given class.
    function gateOf(cls: string): HTMLElement | null {
      return (
        container
          .querySelector<HTMLElement>(`.${cls}`)
          ?.closest<HTMLElement>('[data-gate-open]') ?? null
      );
    }

    const routes: Routes = {
      'overview': () => m('.overview', 'Overview'),
      'object/:id': ({params}) => m('.object', params.id),
    };

    test('keeps previously matched routes mounted but hidden', () => {
      render({path: '/overview', routes, fallback, cached: true});
      const overviewEl = container.querySelector('.overview');
      expect(gateOf('overview')?.dataset.gateOpen, dumpDom()).toBe('true');

      render({path: '/object/1', routes, fallback, cached: true});
      // Same DOM node, still connected, but its gate is closed.
      expect(container.querySelector('.overview'), dumpDom()).toBe(overviewEl);
      expect(gateOf('overview')?.dataset.gateOpen, dumpDom()).toBe('false');
      expect(gateOf('overview')?.style.display, dumpDom()).toBe('none');
      expect(gateOf('object')?.dataset.gateOpen, dumpDom()).toBe('true');

      render({path: '/overview', routes, fallback, cached: true});
      expect(container.querySelector('.overview'), dumpDom()).toBe(overviewEl);
      expect(gateOf('overview')?.dataset.gateOpen, dumpDom()).toBe('true');
      expect(gateOf('object')?.dataset.gateOpen, dumpDom()).toBe('false');
    });

    test('preserves component state across navigation', () => {
      let inits = 0;
      const Stateful: m.Component = {
        oninit: () => {
          inits++;
        },
        view: () => m('.stateful'),
      };
      const statefulRoutes: Routes = {
        stateful: () => m(Stateful),
        other: () => m('.other'),
      };

      render({path: '/stateful', routes: statefulRoutes, cached: true});
      render({path: '/other', routes: statefulRoutes, cached: true});
      render({path: '/stateful', routes: statefulRoutes, cached: true});
      expect(inits).toBe(1);
    });

    test('caches per pattern and re-renders with new params', () => {
      render({path: '/object/1', routes, fallback, cached: true});
      const objectEl = container.querySelector('.object');
      expect(objectEl?.textContent, dumpDom()).toBe('1');

      render({path: '/object/2', routes, fallback, cached: true});
      const objects = container.querySelectorAll('.object');
      expect(objects.length, dumpDom()).toBe(1);
      expect(objects[0], dumpDom()).toBe(objectEl);
      expect(objects[0].textContent, dumpDom()).toBe('2');
    });

    test('renders cached routes in declaration order', () => {
      render({path: '/object/1', routes, fallback, cached: true});
      render({path: '/overview', routes, fallback, cached: true});
      const order = Array.from(
        container.querySelectorAll('.overview, .object'),
      ).map((el) => el.className);
      expect(order, dumpDom()).toEqual(['overview', 'object']);
    });

    test('renders the fallback alongside hidden cached routes', () => {
      render({path: '/overview', routes, fallback, cached: true});
      render({path: '/nope', routes, fallback, cached: true});
      expect(textOf('fallback'), dumpDom()).toBe('Not found');
      expect(gateOf('fallback'), dumpDom()).toBeNull();
      expect(gateOf('overview')?.dataset.gateOpen, dumpDom()).toBe('false');
    });

    test('does not cache the fallback', () => {
      render({path: '/nope', routes, fallback, cached: true});
      expect(textOf('fallback'), dumpDom()).toBe('Not found');

      render({path: '/overview', routes, fallback, cached: true});
      expect(textOf('fallback'), dumpDom()).toBeUndefined();
    });

    test('does not re-render hidden routes', () => {
      const overview = vi.fn(() => m('.overview'));
      const cachedRoutes: Routes = {...routes, overview};
      render({path: '/overview', routes: cachedRoutes, cached: true});
      expect(overview).toHaveBeenCalledTimes(1);

      render({path: '/object/1', routes: cachedRoutes, cached: true});
      render({path: '/object/2', routes: cachedRoutes, cached: true});
      expect(overview).toHaveBeenCalledTimes(1);
    });

    test('drops the cache when cached is turned off', () => {
      render({path: '/overview', routes, fallback, cached: true});
      render({path: '/object/1', routes, fallback, cached: false});
      expect(container.querySelector('.overview'), dumpDom()).toBeNull();
      expect(textOf('object'), dumpDom()).toBe('1');
    });
  });
});
