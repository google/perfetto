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
import {z} from 'zod';
import {getPath, Path, setPath} from './object_utils';

export type Edit<T> = (draft: Draft<T>) => void;

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

export interface Store<T> {
  /**
   * Access the immutable state of this store. The state is parsed through the
   * schema on each access (with caching), so default values are automatically
   * applied and invalid data is coerced or replaced with defaults.
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
   * specific subtree of its parent store. Reads and writes are channelled
   * through to the parent store, with the Zod schema validating and providing
   * defaults on each read.
   *
   * The schema is used to:
   * - Validate incoming state from the parent store
   * - Provide default values for missing fields (via z.default())
   * - Coerce values when possible (e.g., string to number)
   * - Fall back to a fully default state if validation fails entirely
   *
   * Sub-stores may be created over the top of subtrees which are not yet fully
   * defined. The state is written to the parent store on first edit. The
   * sub-store can also deal with the underlying subtree becoming undefined
   * again at some point in the future, and so is robust to unpredictable
   * changes to the root store.
   *
   * @template S The Zod schema type.
   * @param path The path to the subtree this sub-store is based on.
   * @param schema A Zod schema that defines the shape of the state, including
   * any default values. Use z.default() on fields to handle missing data.
   * @example
   * const MyStateSchema = z.object({
   *   count: z.number().default(0),
   *   name: z.string().default(''),
   * });
   *
   * const store = createStore<Record<string, unknown>>({});
   * const subStore = store.createSubStore(['myPlugin'], MyStateSchema);
   * // subStore.state is typed as {count: number, name: string}
   * // Missing fields get their default values automatically
   * @returns {Store<z.output<S>>} The newly created sub-store.
   */
  createSubStore<S extends z.ZodType>(
    path: Path,
    schema: S,
  ): Store<z.output<S>>;
}

/**
 * This class implements a standalone store (i.e. one that does not depend on a
 * subtree of another store).
 * @template T The type of the store's state.
 */
class RootStore<T> implements Store<T> {
  private internalState: T;

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
    const newState = edits.reduce((state, edit) => {
      return produce(state, edit);
    }, this.internalState);

    this.internalState = newState;
  }

  createSubStore<S extends z.ZodType>(
    path: Path,
    schema: S,
  ): Store<z.output<S>> {
    // Cast needed because ZodType<T> uses T for output, but z.output<S>
    // extracts it differently. The runtime behavior is correct.
    return new SubStore<z.output<S>, T>(
      this,
      path,
      schema as z.ZodType<z.output<S>>,
    );
  }
}

/**
 * This class implements a sub-store, one that is based on a subtree of another
 * store. The parent store can be a root level store or another sub-store.
 *
 * This particular implementation of a sub-tree implements a write-through cache
 * style implementation. The sub-store's state is cached internally and all
 * edits are written through to the parent store with a best-effort approach.
 * If the subtree does not exist in the parent store, an error is printed to
 * the console but the operation is still treated as a success.
 *
 * State is parsed through the Zod schema on each read, with caching to avoid
 * redundant parsing when the parent state hasn't changed. This means:
 * - Default values are applied automatically for missing fields
 * - Invalid data is coerced when possible, or falls back to defaults
 * - Schema changes (e.g., adding a new field with .default()) apply immediately
 *
 * @template T The type of the sub-store's state.
 * @template ParentT The type of the parent store's state.
 */
class SubStore<T, ParentT> implements Store<T> {
  private cachedParentState: unknown;
  private cachedParsedState: T;

  constructor(
    private readonly parentStore: Store<ParentT>,
    private readonly path: Path,
    private readonly schema: z.ZodType<T>,
  ) {
    this.cachedParentState = getPath<unknown>(
      this.parentStore.state,
      this.path,
    );
    this.cachedParsedState = produce(
      this.parse(this.cachedParentState),
      () => {},
    );
  }

  /**
   * Parse raw state through the schema, falling back to defaults on failure.
   */
  private parse(raw: unknown): T {
    const result = this.schema.safeParse(raw ?? {});
    if (result.success) {
      return result.data;
    }
    // Schema validation failed entirely - fall back to parsing empty object
    // which will use all default values from the schema
    console.warn(
      `Store state at path [${this.path.join(', ')}] failed validation, using defaults:`,
      result.error.format(),
    );
    const fallback = this.schema.safeParse({});
    if (fallback.success) {
      return fallback.data;
    }
    // This shouldn't happen if the schema has proper defaults, but if it does,
    // we have no choice but to throw
    throw new Error(
      `Store schema at path [${this.path.join(', ')}] has no valid default state`,
    );
  }

  get state(): T {
    const parentState = getPath<unknown>(this.parentStore.state, this.path);
    if (this.cachedParentState === parentState) {
      return this.cachedParsedState;
    }
    // Parent state changed - re-parse through schema
    this.cachedParentState = parentState;
    this.cachedParsedState = produce(this.parse(parentState), () => {});
    return this.cachedParsedState;
  }

  edit(edit: Edit<T> | Edit<T>[]): void {
    if (Array.isArray(edit)) {
      this.applyEdits(edit);
    } else {
      this.applyEdits([edit]);
    }
  }

  private applyEdits(edits: Edit<T>[]): void {
    // Start from current parsed state (with defaults applied)
    const newState = edits.reduce((state, edit) => {
      return produce(state, edit);
    }, this.state);

    // Update cache
    this.cachedParentState = newState;
    this.cachedParsedState = newState;

    // Write through to parent
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
  }

  createSubStore<S extends z.ZodType>(
    path: Path,
    schema: S,
  ): Store<z.output<S>> {
    return new SubStore<z.output<S>, T>(
      this,
      path,
      schema as z.ZodType<z.output<S>>,
    );
  }
}
