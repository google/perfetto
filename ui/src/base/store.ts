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

import {produce, Draft} from 'immer';
import {getPath, Path, setPath} from './object_utils';

export type Migrate<T> = (init: unknown) => T;
export type Edit<T> = (draft: Draft<T>) => void;
export type Callback<T> = (store: Store<T>, previous: T) => void;

/**
 * Create a new root-level store.
 *
 * @template T The type of this store's state.
 * @param {T} initialState Initial state of the store.
 * @returns {Store<T>} The newly created store.
 */
export function createStore<T>(initialState: T): Store<T> {
  return new RootStore<T>(initialState);
}

export interface Store<T> extends Disposable {
  /**
   * Access the immutable state of this store.
   */
  get state(): T;

  /**
   * Mutate the store's state.
   *
   * @param edits The edit (or edits) to the store.
   */
  edit(edits: Edit<T> | Edit<T>[]): void;

  /**
   * Create a sub-store from a subtree of the state from this store.
   *
   * The returned store looks and feels like a regular store but acts only on a
   * specific subtree of its parent store. Reads are writes are channelled
   * through to the parent store via the |migrate| function.
   *
   * |migrate| is called the first time we access our sub-store's state and
   * whenever the subtree changes in the root store.
   * This migrate function takes the state of the subtree from the sub-store's
   * parent store which has unknown type and is responsible for returning a
   * value whose type matches that of the sub-store's state.
   *
   * Sub-stores may be created over the top of subtrees which are not yet fully
   * defined. The state is written to the parent store on first edit. The
   * sub-store can also deal with the underlying subtree becoming undefined
   * again at some point in the future, and so is robust to unpredictable
   * changes to the root store.
   *
   * @template U The type of the sub-store's state.
   * @param path The path to the subtree this sub-store is based on.
   * @example
   * // Given a store whose state takes the form:
   * {
   *   foo: {
   *     bar: [ {baz: 123}, {baz: 42} ],
   *   },
   * }
   *
   * // A sub-store crated on path: ['foo','bar', 1] would only see the state:
   * {
   *   baz: 42,
   * }
   * @param migrate A function used to migrate from the parent store's subtree
   * to the sub-store's state.
   * @example
   * interface RootState {dict: {[key: string]: unknown}};
   * interface SubState {foo: string};
   *
   * const store = createStore({dict: {}});
   * const migrate = (init: unknown) => (init ?? {foo: 'bar'}) as SubState;
   * const subStore = store.createSubStore(store, ['dict', 'foo'], migrate);
   * // |dict['foo']| will be created the first time we edit our sub-store.
   * Warning: Migration functions should properly validate the incoming state.
   * Blindly using type assertions can lead to instability.
   * @returns {Store<U>} The newly created sub-store.
   */
  createSubStore<U>(path: Path, migrate: Migrate<U>): Store<U>;

  /**
   * Subscribe for notifications when any edits are made to this store.
   *
   * @param callback The function to be called.
   * @returns When this is disposed, the subscription is removed.
   */
  subscribe(callback: Callback<T>): Disposable;
}

/**
 * This class implements a standalone store (i.e. one that does not depend on a
 * subtree of another store).
 * @template T The type of the store's state.
 */
class RootStore<T> implements Store<T> {
  private internalState: T;
  private subscriptions = new Set<Callback<T>>();

  constructor(initialState: T) {
    // Run initial state through immer to take advantage of auto-freezing
    this.internalState = produce(initialState, () => {});
  }

  get state() {
    return this.internalState;
  }

  edit(edit: Edit<T> | Edit<T>[]): void {
    if (Array.isArray(edit)) {
      this.applyEdits(edit);
    } else {
      this.applyEdits([edit]);
    }
  }

  private applyEdits(edits: Edit<T>[]): void {
    const originalState = this.internalState;

    const newState = edits.reduce((state, edit) => {
      return produce(state, edit);
    }, originalState);

    this.internalState = newState;

    // Notify subscribers
    this.subscriptions.forEach((sub) => {
      sub(this, originalState);
    });
  }

  createSubStore<U>(path: Path, migrate: Migrate<U>): Store<U> {
    return new SubStore(this, path, migrate);
  }

  subscribe(callback: Callback<T>): Disposable {
    this.subscriptions.add(callback);
    return {
      [Symbol.dispose]: () => {
        this.subscriptions.delete(callback);
      },
    };
  }

  [Symbol.dispose]() {
    // No-op
  }
}

/**
 * This class implements a sub-store, one that is based on a subtree of another
 * store. The parent store can be a root level store or another sub-store.
 *
 * This particular implementation of a sub-tree implements a write-through cache
 * style implementation. The sub-store's state is cached internally and all
 * edits are written through to the parent store as with a best-effort approach.
 * If the subtree does not exist in the parent store, an error is printed to
 * the console but the operation is still treated as a success.
 *
 * @template T The type of the sub-store's state.
 * @template ParentT The type of the parent store's state.
 */
class SubStore<T, ParentT> implements Store<T> {
  private parentState: unknown;
  private cachedState: T;
  private parentStoreSubscription: Disposable;
  private subscriptions = new Set<Callback<T>>();

  constructor(
    private readonly parentStore: Store<ParentT>,
    private readonly path: Path,
    private readonly migrate: (init: unknown) => T,
  ) {
    this.parentState = getPath<unknown>(this.parentStore.state, this.path);

    // Run initial state through immer to take advantage of auto-freezing
    this.cachedState = produce(migrate(this.parentState), () => {});

    // Subscribe to parent store changes.
    this.parentStoreSubscription = this.parentStore.subscribe(() => {
      const newRootState = getPath<unknown>(this.parentStore.state, this.path);
      if (newRootState !== this.parentState) {
        this.subscriptions.forEach((callback) => {
          callback(this, this.cachedState);
        });
      }
    });
  }

  get state(): T {
    const parentState = getPath<unknown>(this.parentStore.state, this.path);
    if (this.parentState === parentState) {
      return this.cachedState;
    } else {
      this.parentState = parentState;
      return (this.cachedState = produce(this.cachedState, () => {
        return this.migrate(parentState);
      }));
    }
  }

  edit(edit: Edit<T> | Edit<T>[]): void {
    if (Array.isArray(edit)) {
      this.applyEdits(edit);
    } else {
      this.applyEdits([edit]);
    }
  }

  private applyEdits(edits: Edit<T>[]): void {
    const originalState = this.cachedState;

    const newState = edits.reduce((state, edit) => {
      return produce(state, edit);
    }, originalState);

    this.parentState = newState;
    try {
      this.parentStore.edit((draft) => {
        setPath(draft, this.path, newState);
      });
    } catch (error) {
      if (error instanceof TypeError) {
        console.warn('Failed to update parent store at ', this.path);
      } else {
        throw error;
      }
    }

    this.cachedState = newState;

    this.subscriptions.forEach((sub) => {
      sub(this, originalState);
    });
  }

  createSubStore<SubtreeState>(
    path: Path,
    migrate: Migrate<SubtreeState>,
  ): Store<SubtreeState> {
    return new SubStore(this, path, migrate);
  }

  subscribe(callback: Callback<T>): Disposable {
    this.subscriptions.add(callback);
    return {
      [Symbol.dispose]: () => {
        this.subscriptions.delete(callback);
      },
    };
  }

  [Symbol.dispose]() {
    this.parentStoreSubscription[Symbol.dispose]();
  }
}
