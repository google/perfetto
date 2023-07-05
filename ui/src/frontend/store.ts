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

import produce, {Draft} from 'immer';

import {Disposable} from '../base/disposable';
import {lookupPath, Path} from '../base/object_utils';
import {exists} from '../base/utils';

export class StoreError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'StoreError';
  }
}

// Describes a generic edit on a store of type T.
export type Edit<T> = (draft: Draft<T>) => void;

// Describes a callback from a store notifying subscribers of state changes.
export type SubscriptionCallback<T> = (state: T, oldState: T) => void;

// Describes a generic store of type T where T is the type of the state object
// the store manages.
// A store can be edited, subscribed to, and have proxies created from it.
export interface Store<T> extends Disposable {
  // Access to the store's state.
  // This state should be treated as immutable. It may be frozen in the future.
  get state(): T;

  // Apply one or multiple edits. Multiple edits are applied atomically.
  // If any edits throw, the state is left unchanged.
  // Subscribers are only called after all edits are made.
  // Note: Purposely avoided using ...rest parameters here to avoid overflowing
  // the stack when passing huge arrays of edits.
  edit(edits: Edit<T>|Edit<T>[]): void;

  // Create a new proxy store on a sub-tree of the state.
  // This proxy store looks and feels like a regular store, but acts only on a
  // specific subtree within its root store.
  //
  // The path doesn't need to exist in the object, but operations will be
  // limited until it does.
  // When the path is missing:
  //  - Accessing state on the proxy will throw.
  //  - Calling edit() on the proxy will throw.
  //  - The proxy won't notify subscribers the first time the subtree becomes
  //    defined, or when it becomes undefined.
  createProxy<SubStateT>(path: Path): Store<SubStateT>;

  // Register to be notified when edits are made to the store.
  // The callback will be called whenever edits are made to the state managed by
  // this store.
  // Callbacks are passed the old state (state of the store before applying the
  // edit(s)) and the new (current) state.
  subscribe(callback: SubscriptionCallback<T>): Disposable;
}

// Factory method to create a new store, which lets the underlying store
// implementation change arbitrarily.
export function createStore<T>(initialState: T) {
  return new StoreImpl<T>(initialState);
}

// Root store implementaiton.
class StoreImpl<T> implements Store<T> {
  private _state: T;
  private subscriptions = new Set<SubscriptionCallback<T>>();

  constructor(initialState: T) {
    this._state = initialState;
  }

  dispose() {
    // No-op
  }

  get state() {
    return this._state;
  }

  edit(edit: Edit<T>|Edit<T>[]): void {
    if (Array.isArray(edit)) {
      return this.applyEdits(edit);
    } else {
      return this.applyEdits([edit]);
    }
  }

  private applyEdits(edits: Edit<T>[]): void {
    const oldState = this._state;
    let newState = oldState;

    edits.forEach((edit) => {
      newState = produce(newState, edit);
    });

    // Notify subscribers only if the state has changed.
    if (oldState !== newState) {
      this.subscriptions.forEach((sub) => {
        sub(newState, oldState);
      });

      // It's important this is done last in order to keep updates atomic.
      this._state = newState;
    }
  }

  createProxy<ProxyT>(path: Path): Store<ProxyT> {
    return new ProxyStoreImpl<T, ProxyT>(this, path);
  }

  subscribe(callback: SubscriptionCallback<T>): Disposable {
    this.subscriptions.add(callback);
    return {
      dispose: () => {
        this.subscriptions.delete(callback);
      },
    };
  }
}

// A proxy store implemenation.
// This proxy implementation subscribes to the root store and thus must be
// disposed of properly by calling dispose() to avoid leaks.
// All edits are modified to operate on the subtree and passed back to the root
// store.
export class ProxyStoreImpl<RootT, T> implements Store<T> {
  private subscriptions = new Set<SubscriptionCallback<T>>();
  private rootSubscription;
  private rootStore?: Store<RootT>;

  constructor(
      rootStore: Store<RootT>,
      private path: Path,
  ) {
    this.rootStore = rootStore;
    this.rootSubscription = rootStore.subscribe(this.rootUpdateHandler);
  }

  dispose() {
    this.rootSubscription.dispose();
    this.rootStore = undefined;
  }

  private rootUpdateHandler = (newState: RootT, oldState: RootT) => {
    const newSubState = lookupPath<T, RootT>(newState, this.path);
    const oldSubState = lookupPath<T, RootT>(oldState, this.path);
    if (exists(newSubState) && exists(oldSubState) &&
        newSubState != oldSubState) {
      this.subscriptions.forEach((subscription) => {
        subscription(newSubState, oldSubState);
      });
    }
  };

  get state(): T {
    if (!this.rootStore) {
      throw new StoreError('Proxy store is no longer useable');
    }

    const state = lookupPath<T, RootT>(this.rootStore.state, this.path);
    if (state === undefined) {
      throw new StoreError(`No such subtree: ${this.path}`);
    }

    return state;
  }

  edit(edit: Edit<T>|Edit<T>[]): void {
    if (Array.isArray(edit)) {
      this.applyEdits(edit);
    } else {
      this.applyEdits([edit]);
    }
  }

  private applyEdits(edits: Edit<T>[]): void {
    if (!this.rootStore) {
      throw new StoreError('Proxy store is no longer useable');
    }

    // Transform edits to work on the root store.
    const rootEdits = edits.map(
        (edit) => (state: Draft<RootT>) => {
          // Extract subtree and apply edits to it.
          const subtree = lookupPath<Draft<T>, Draft<RootT>>(state, this.path);
          if (subtree === undefined) {
            throw new StoreError(
                `Unable to edit missing subtree: ${this.path}`);
          }
          edit(subtree);
        },
    );

    // Apply edits to the root store.
    this.rootStore.edit(rootEdits);
  }

  createProxy<NewSubStateT>(path: Path): Store<NewSubStateT> {
    if (!this.rootStore) {
      throw new StoreError('Proxy store is no longer useable');
    }

    const fullPath = [...this.path, ...path];
    return new ProxyStoreImpl<RootT, NewSubStateT>(this.rootStore, fullPath);
  }

  subscribe(callback: SubscriptionCallback<T>): Disposable {
    this.subscriptions.add(callback);
    return {
      dispose: () => {
        this.subscriptions.delete(callback);
      },
    };
  }
}
