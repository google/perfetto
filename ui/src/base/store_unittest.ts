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

import {Draft} from 'immer';
import {z} from 'zod';
import {createStore} from './store';

// Zod schemas for test types
const BarSchema = z.object({
  value: z.number().default(456),
});

const FooSchema = z.object({
  counter: z.number().default(123),
  nested: BarSchema.prefault({}),
});

type Foo = z.output<typeof FooSchema>;

interface State {
  foo: Foo;
}

const initialState: State = {
  foo: {
    counter: 0,
    nested: {
      value: 42,
    },
  },
};

describe('root store', () => {
  test('edit', () => {
    const store = createStore(initialState);
    store.edit((draft) => {
      draft.foo.counter += 123;
    });

    expect(store.state).toEqual({
      foo: {
        counter: 123,
        nested: {
          value: 42,
        },
      },
    });
  });

  test('state [in]equality', () => {
    const store = createStore(initialState);
    store.edit((draft) => {
      draft.foo.counter = 88;
    });
    expect(store.state).not.toBe(initialState);
    expect(store.state.foo).not.toBe(initialState.foo);
    expect(store.state.foo.nested).toBe(initialState.foo.nested);
  });

  it('can take multiple edits at once', () => {
    const store = createStore(initialState);

    store.edit([
      (draft) => {
        draft.foo.counter += 10;
      },
      (draft) => {
        draft.foo.counter += 10;
      },
    ]);

    expect(store.state).toEqual({
      foo: {
        counter: 20,
        nested: {
          value: 42,
        },
      },
    });
  });

  it('can support a huge number of edits', () => {
    const store = createStore(initialState);
    const N = 100_000;
    const edits = Array(N).fill((draft: Draft<State>) => {
      draft.foo.counter++;
    });
    store.edit(edits);
    expect(store.state.foo.counter).toEqual(N);
  });
});

describe('sub-store', () => {
  test('edit', () => {
    const store = createStore(initialState);
    const subStore = store.createSubStore(['foo'], FooSchema);

    subStore.edit((draft) => {
      draft.counter += 1;
    });

    expect(subStore.state).toEqual({
      counter: 1,
      nested: {
        value: 42,
      },
    });

    expect(store.state).toEqual({
      foo: {
        counter: 1,
        nested: {
          value: 42,
        },
      },
    });
  });

  test('edit from root store', () => {
    const store = createStore(initialState);
    const subStore = store.createSubStore(['foo'], FooSchema);

    store.edit((draft) => {
      draft.foo.counter += 1;
    });

    expect(subStore.state).toEqual({
      counter: 1,
      nested: {
        value: 42,
      },
    });
  });

  it('can create more substores and edit', () => {
    const store = createStore(initialState);
    const fooState = store.createSubStore(['foo'], FooSchema);
    const nestedStore = fooState.createSubStore(['nested'], BarSchema);

    nestedStore.edit((draft) => {
      draft.value += 1;
    });

    expect(nestedStore.state).toEqual({
      value: 43,
    });
  });

  it('handles reading when path does not exist in root store', () => {
    const store = createStore(initialState);

    // This target node is missing - baz doesn't exist in State
    // With Zod schemas, missing paths get default values from the schema
    const subStore = store.createSubStore(['baz'], FooSchema);
    expect(subStore.state).toEqual({
      counter: 123,
      nested: {
        value: 456,
      },
    });
  });

  it("handles edit when path doesn't exist in root store", () => {
    const store = createStore(initialState);

    // This target node is missing - baz doesn't exist in State
    // With Zod schemas, we get defaults and can edit them
    const subStore = store.createSubStore(['baz', 'quux'], FooSchema);

    // Edits should work just fine, but the root store will not be modified
    // because the intermediate path doesn't exist.
    subStore.edit((draft) => {
      draft.counter += 1;
    });

    // The substore's cached state is updated
    expect(subStore.state.counter).toBe(124);
  });

  it('immutable [in]equality works', () => {
    const store = createStore(initialState);
    const subStore = store.createSubStore(['foo'], FooSchema);
    const before = subStore.state;

    subStore.edit((draft) => {
      draft.counter += 1;
    });

    const after = subStore.state;

    // something has changed so root should not equal
    expect(before).not.toBe(after);

    // nested has not changed and so should be the before version.
    expect(before.nested).toBe(after.nested);
  });

  it('unrelated state refs are still equal when modified from root store', () => {
    const store = createStore(initialState);
    const subStore = store.createSubStore(['foo'], FooSchema);
    const before = subStore.state;

    // Check that unrelated state is still the same even though subtree is
    // modified from the root store
    store.edit((draft) => {
      draft.foo.counter = 1234;
    });

    // With Zod schema parsing and immer, structural sharing is preserved
    expect(before.nested).toBe(subStore.state.nested);
    expect(subStore.state.counter).toBe(1234);
  });

  it('works when underlying state is undefined', () => {
    interface RootState {
      dict: {[key: string]: unknown};
    }

    const ProxyStateSchema = z.object({
      bar: z.string().default('bar'),
    });

    const store = createStore<RootState>({dict: {}});
    const subStore = store.createSubStore(['dict', 'foo'], ProxyStateSchema);

    // Check initial default values work, yet underlying store is untouched
    expect(subStore.state.bar).toBe('bar');
    expect(store.state.dict['foo']).toBe(undefined);

    // Check updates work
    subStore.edit((draft) => {
      draft.bar = 'baz';
    });
    expect(subStore.state.bar).toBe('baz');
    expect(
      (store.state.dict['foo'] as z.output<typeof ProxyStateSchema>).bar,
    ).toBe('baz');
  });

  test('chained substores', () => {
    interface State {
      dict: {[key: string]: unknown};
    }

    const BarStateSchema = z.object({
      baz: z.string().default('abc'),
    });

    const FooStateSchema = z.object({
      bar: BarStateSchema.prefault({}),
    });

    const store = createStore<State>({dict: {}});

    const fooStore = store.createSubStore(['dict', 'foo'], FooStateSchema);

    const subFooStore = fooStore.createSubStore(['bar'], BarStateSchema);

    // Since the entry for 'foo' will be undefined in the dict, we expect the
    // schema defaults to be applied, and thus the state of the subFooStore
    // will be the default bar state.
    expect(subFooStore.state).toEqual({baz: 'abc'});
  });

  test('schema provides defaults for missing fields', () => {
    interface RootState {
      plugins: {[key: string]: unknown};
    }

    const PluginStateSchema = z.object({
      count: z.number().default(0),
      name: z.string().default('default'),
      enabled: z.boolean().default(true),
    });

    const store = createStore<RootState>({plugins: {}});
    const pluginStore = store.createSubStore(
      ['plugins', 'myPlugin'],
      PluginStateSchema,
    );

    // All defaults should be applied
    expect(pluginStore.state).toEqual({
      count: 0,
      name: 'default',
      enabled: true,
    });

    // Partial updates preserve other defaults
    pluginStore.edit((draft) => {
      draft.count = 5;
    });

    expect(pluginStore.state).toEqual({
      count: 5,
      name: 'default',
      enabled: true,
    });
  });

  test('schema coerces and validates partial data', () => {
    interface RootState {
      data: unknown;
    }

    const DataSchema = z.object({
      value: z.number().default(100),
      label: z.string().default('untitled'),
    });

    // Simulate loading from permalink with only partial data
    const store = createStore<RootState>({
      data: {value: 42}, // label is missing
    });

    const dataStore = store.createSubStore(['data'], DataSchema);

    // value should be preserved, label should get default
    expect(dataStore.state).toEqual({
      value: 42,
      label: 'untitled',
    });
  });
});
