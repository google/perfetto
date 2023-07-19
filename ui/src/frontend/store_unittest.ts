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

import {createStore, StoreError} from './store';
import {using} from '../base/disposable';

interface NestedState {
  value: number;
}

interface Foo {
  counter: number;
  nested: NestedState;
}

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

describe('store', () => {
  it('can initialise', () => {
    const store = createStore(initialState);
    expect(store.state).toEqual(initialState);
  });

  test('old state !== state after edits', () => {
    const store = createStore(initialState);
    store.edit((draft) => {
      draft.foo.nested.value = 88;
    });
    expect(store.state).not.toEqual(initialState);
  });

  it('can edit', () => {
    const store = createStore(initialState);
    store.edit((draft) => {
      draft.foo.counter += 1;
    });

    expect(store.state).toEqual({
      ...initialState,
      foo: {
        ...initialState.foo,
        counter: 1,
      },
    });
  });

  it('can take multiple edits', () => {
    const store = createStore(initialState);
    const callback = jest.fn();

    store.subscribe(callback);

    store.edit([
      (draft) => {
        draft.foo.counter += 1;
      },
      (draft) => {
        draft.foo.nested.value += 1;
      },
    ]);

    expect(callback).toHaveBeenCalledTimes(1);
    expect(callback).toHaveBeenCalledWith(
        {
          foo: {
            counter: 1,
            nested: {
              value: 43,
            },
          },
        },
        initialState);

    expect(store.state).toEqual({
      foo: {
        counter: 1,
        nested: {
          value: 43,
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

  it('notifies subscribers', () => {
    const store = createStore(initialState);
    const callback = jest.fn();

    store.subscribe(callback);

    store.edit((draft) => {
      draft.foo.counter += 1;
    });

    expect(callback).toHaveBeenCalledTimes(1);
    expect(callback).toHaveBeenCalledWith(
        {
          foo: {
            ...initialState.foo,
            counter: 1,
          },
        },
        initialState);
  });

  it('does not notify unsubscribed subscribers', () => {
    const store = createStore(initialState);
    const callback = jest.fn();

    // Subscribe then immediately unsubscribe
    using(store.subscribe(callback));

    // Make an arbitrary edit
    store.edit((draft) => {
      draft.foo.counter += 1;
    });

    expect(callback).not.toHaveBeenCalled();
  });
});

describe('proxy store', () => {
  it('can initialise and edit', () => {
    const store = createStore(initialState);
    const fooState = store.createProxy<Foo>(['foo']);

    fooState.edit((draft) => {
      draft.counter += 1;
    });

    expect(fooState.state).toEqual({
      counter: 1,
      nested: {
        value: 42,
      },
    });
  });

  it('can create more proxies and edit', () => {
    const store = createStore(initialState);
    const fooState = store.createProxy<Foo>(['foo']);
    const nestedStore = fooState.createProxy<NestedState>(['nested']);

    nestedStore.edit((draft) => {
      draft.value += 1;
    });

    expect(nestedStore.state).toEqual({
      value: 43,
    });
  });

  it('notifies subscribers', () => {
    const store = createStore(initialState);
    const fooState = store.createProxy<Foo>(['foo']);
    const callback = jest.fn();

    fooState.subscribe(callback);

    store.edit((draft) => {
      draft.foo.counter += 1;
    });

    expect(callback).toHaveBeenCalledTimes(1);
    expect(callback).toHaveBeenCalledWith(
        {
          ...initialState.foo,
          counter: 1,
        },
        initialState.foo);
  });

  it('does not notify unsubscribed subscribers', () => {
    const store = createStore(initialState);
    const fooState = store.createProxy<Foo>(['foo']);
    const callback = jest.fn();

    // Subscribe then immediately unsubscribe
    using(store.subscribe(callback));

    // Make an arbitrary edit
    fooState.edit((draft) => {
      draft.counter += 1;
    });

    expect(callback).not.toHaveBeenCalled();
  });

  it('throws on state access when path doesn\'t exist', () => {
    const store = createStore(initialState);

    // This path is incorrect - baz doesn't exist in State
    const fooStore = store.createProxy<Foo>(['baz']);

    expect(() => {
      fooStore.state;
    }).toThrow(StoreError);
  });

  it('throws on edit when path doesn\'t exist', () => {
    const store = createStore(initialState);

    // This path is incorrect - baz doesn't exist in State
    const fooState = store.createProxy<Foo>(['baz']);

    expect(() => {
      fooState.edit((draft) => {
        draft.counter += 1;
      });
    }).toThrow(StoreError);
  });

  it('notifies when relevant edits are made from root store', () => {
    const store = createStore(initialState);
    const fooState = store.createProxy<Foo>(['foo']);
    const callback = jest.fn();

    // Subscribe on the proxy store
    fooState.subscribe(callback);

    // Edit the root store
    store.edit((draft) => {
      draft.foo.counter++;
    });

    // Expect proxy callback called with correct subtree
    expect(callback).toHaveBeenCalled();
    expect(callback).toHaveBeenCalledWith(
        {
          ...initialState.foo,
          counter: 1,
        },
        initialState.foo);
  });

  it('ignores irrrelevant edits from the root store', () => {
    const store = createStore(initialState);
    const nestedStore = store.createProxy<NestedState>(['foo', 'nested']);
    const callback = jest.fn();

    // Subscribe on the proxy store
    nestedStore.subscribe(callback);

    // Edit an irrelevant subtree on the root store
    store.edit((draft) => {
      draft.foo.counter++;
    });

    // Ensure proxy callback hasn't been called
    expect(callback).not.toHaveBeenCalled();
  });

  it('notifies subscribers', () => {
    const store = createStore(initialState);
    const fooState = store.createProxy<Foo>(['foo']);
    const callback = jest.fn();

    fooState.subscribe(callback);

    store.edit((draft) => {
      draft.foo.counter += 1;
    });

    expect(callback).toHaveBeenCalledTimes(1);
    expect(callback).toHaveBeenCalledWith(
        {
          ...initialState.foo,
          counter: 1,
        },
        initialState.foo);
  });

  it('does not notify unsubscribed subscribers', () => {
    const store = createStore(initialState);
    const fooState = store.createProxy<Foo>(['foo']);
    const callback = jest.fn();

    // Subscribe then immediately unsubscribe
    fooState.subscribe(callback).dispose();

    // Make an arbitrary edit
    fooState.edit((draft) => {
      draft.counter += 1;
    });

    expect(callback).not.toHaveBeenCalled();
  });

  it('throws on state access when path doesn\'t exist', () => {
    const store = createStore(initialState);

    // This path is incorrect - baz doesn't exist in State
    const fooStore = store.createProxy<Foo>(['baz']);

    expect(() => {
      fooStore.state;
    }).toThrow(StoreError);
  });

  it('throws on edit when path doesn\'t exist', () => {
    const store = createStore(initialState);

    // This path is incorrect - baz doesn't exist in State
    const fooState = store.createProxy<Foo>(['baz']);

    expect(() => {
      fooState.edit((draft) => {
        draft.counter += 1;
      });
    }).toThrow(StoreError);
  });

  it('notifies when relevant edits are made from root store', () => {
    const store = createStore(initialState);
    const fooState = store.createProxy<Foo>(['foo']);
    const callback = jest.fn();

    // Subscribe on the proxy store
    fooState.subscribe(callback);

    // Edit the root store
    store.edit((draft) => {
      draft.foo.counter++;
    });

    // Expect proxy callback called with correct subtree
    expect(callback).toHaveBeenCalled();
    expect(callback).toHaveBeenCalledWith(
        {
          ...initialState.foo,
          counter: 1,
        },
        initialState.foo);
  });

  it('ignores irrrelevant edits from the root store', () => {
    const store = createStore(initialState);
    const nestedStore = store.createProxy<NestedState>(['foo', 'nested']);
    const callback = jest.fn();

    // Subscribe on the proxy store
    nestedStore.subscribe(callback);

    // Edit an irrelevant subtree on the root store
    store.edit((draft) => {
      draft.foo.counter++;
    });

    // Ensure proxy callback hasn't been called
    expect(callback).not.toHaveBeenCalled();
  });
});
