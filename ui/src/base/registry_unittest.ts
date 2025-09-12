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

import {Registry} from './registry';

interface Registrant {
  kind: string;
  n: number;
}

test('registry returns correct registrant', () => {
  const registry = Registry.kindRegistry<Registrant>();

  const a: Registrant = {kind: 'a', n: 1};
  const b: Registrant = {kind: 'b', n: 2};
  registry.register(a);
  registry.register(b);

  expect(registry.get('a')).toBe(a);
  expect(registry.get('b')).toBe(b);
});

test('registry throws error on kind collision', () => {
  const registry = Registry.kindRegistry<Registrant>();

  const a1: Registrant = {kind: 'a', n: 1};
  const a2: Registrant = {kind: 'a', n: 2};

  registry.register(a1);
  expect(() => registry.register(a2)).toThrow();
});

test('registry throws error on non-existent track', () => {
  const registry = Registry.kindRegistry<Registrant>();
  expect(() => registry.get('foo')).toThrow();
});

test('registry allows iteration', () => {
  const registry = Registry.kindRegistry<Registrant>();
  const a: Registrant = {kind: 'a', n: 1};
  const b: Registrant = {kind: 'b', n: 2};
  registry.register(a);
  registry.register(b);

  const values = [...registry.values()];
  expect(values.length).toBe(2);
  expect(values.includes(a)).toBe(true);
  expect(values.includes(b)).toBe(true);
});

describe('Hierarchical (child) registries', () => {
  test('inheritance of registrations', () => {
    const parent = Registry.kindRegistry<Registrant>();
    const child = parent.createChild();
    const a: Registrant = {kind: 'a', n: 1};
    const b: Registrant = {kind: 'b', n: 2};
    const c: Registrant = {kind: 'c', n: 3};
    parent.register(a);
    child.register(b);
    parent.register(c);

    // Parent and child regs seen from child
    expect(child.get('a')).toBe(a);
    expect(child.get('b')).toBe(b);
    expect(child.get('c')).toBe(c);

    // Parent does not see child's registrations
    expect(() => parent.get('b')).toThrow();
  });

  test('shadowing of inherited registrations', () => {
    const parent = Registry.kindRegistry<Registrant>();
    const child = parent.createChild();
    const a1: Registrant = {kind: 'a', n: 1};
    const a2: Registrant = {kind: 'a', n: 99};
    parent.register(a1);
    child.register(a2);

    // Child shadows parent's registration
    expect(child.get('a')).toBe(a2);
    expect(parent.get('a')).toBe(a1);
  });

  test('does not allow multiple registration of same kind in child', () => {
    const parent = Registry.kindRegistry<Registrant>();
    const child = parent.createChild();
    const b1: Registrant = {kind: 'b', n: 10};
    const b2: Registrant = {kind: 'b', n: 20};
    child.register(b1);
    expect(() => child.register(b2)).toThrow();
  });

  test('inheritance of has()', () => {
    const parent = Registry.kindRegistry<Registrant>();
    const child = parent.createChild();
    const a: Registrant = {kind: 'a', n: 1};
    const b: Registrant = {kind: 'b', n: 2};
    const c: Registrant = {kind: 'c', n: 3};
    parent.register(a);
    child.register(b);
    parent.register(c);

    // Child should have parent's and own keys
    expect(child.has('a')).toBe(true);
    expect(child.has('b')).toBe(true);
    expect(child.has('c')).toBe(true);
    expect(child.has('d')).toBe(false);

    // Parent should not have child's keys
    expect(parent.has('b')).toBe(false);
    expect(parent.has('a')).toBe(true);
    expect(parent.has('c')).toBe(true);
    expect(parent.has('d')).toBe(false);
  });

  test('iteration over registrations', () => {
    // Iteration includes inherited registrations
    const parent = Registry.kindRegistry<Registrant>();
    const child = parent.createChild();
    const a: Registrant = {kind: 'a', n: 1};
    const b: Registrant = {kind: 'b', n: 2};
    const c: Registrant = {kind: 'c', n: 3};
    parent.register(a);
    child.register(b);
    parent.register(c);

    const values = child.valuesAsArray();
    // The child registry should yield both its own and its parent's registrations
    expect(values.length).toBe(3);
    expect(values).toContain(a);
    expect(values).toContain(b);
    expect(values).toContain(c);
  });

  test('iteration does not include parent services overridden by child', () => {
    const parent = Registry.kindRegistry<Registrant>();
    const child = parent.createChild();
    const aParent: Registrant = {kind: 'a', n: 29};
    const aChild: Registrant = {kind: 'a', n: 42}; // Override in child
    const b: Registrant = {kind: 'b', n: 1};
    const c: Registrant = {kind: 'c', n: 19};
    parent.register(aParent);
    parent.register(b);
    child.register(aChild); // Shadows parent's 'a'
    child.register(c);

    const values = child.valuesAsArray();
    expect(values.length).toBe(3);
    expect(values).toContain(c);
    expect(values).toContain(b);
    expect(values).toContain(aChild);
    expect(values).not.toContain(aParent); // The parent's 'a' is hidden
  });

  test('with id', () => {
    const parent = Registry.kindRegistry<Registrant>();
    const child = parent.createChild('child');

    expect(child).toHaveProperty('id', 'child');
  });

  test('without id', () => {
    const parent = Registry.kindRegistry<Registrant>();
    const child = parent.createChild();

    expect(child).not.toHaveProperty('id');
  });
});
