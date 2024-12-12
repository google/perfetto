// Copyright (C) 2024 The Android Open Source Project
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

/**
 * Implementations of DisposableStack and AsyncDisposableStack.
 *
 * These are defined in the "ECMAScript Explicit Resource Management" proposal
 * which is currently at stage 3, which means "No changes to the proposal are
 * expected, but some necessary changes may still occur due to web
 * incompatibilities or feedback from production-grade implementations."
 *
 * Reference
 * - https://github.com/tc39/proposal-explicit-resource-management
 * - https://tc39.es/process-document/
 *
 * These classes are purposely not polyfilled to avoid confusion and aid
 * debug-ability and traceability.
 */

export class DisposableStack implements Disposable {
  private readonly resources: Disposable[];
  private isDisposed = false;

  constructor() {
    this.resources = [];
  }

  use<T extends Disposable | null | undefined>(res: T): T {
    if (res == null) return res;
    this.resources.push(res);
    return res;
  }

  defer(onDispose: () => void) {
    this.resources.push({
      [Symbol.dispose]: onDispose,
    });
  }

  // TODO(stevegolton): Handle error suppression properly
  // https://github.com/tc39/proposal-explicit-resource-management?tab=readme-ov-file#aggregation
  [Symbol.dispose](): void {
    this.isDisposed = true;
    while (true) {
      const res = this.resources.pop();
      if (res === undefined) {
        break;
      }
      res[Symbol.dispose]();
    }
  }

  dispose(): void {
    this[Symbol.dispose]();
  }

  adopt<T>(value: T, onDispose: (value: T) => void): T {
    this.resources.push({
      [Symbol.dispose]: () => onDispose(value),
    });
    return value;
  }

  move(): DisposableStack {
    const other = new DisposableStack();
    for (const res of this.resources) {
      other.resources.push(res);
    }
    this.resources.length = 0;
    return other;
  }

  readonly [Symbol.toStringTag]: string = 'DisposableStack';

  get disposed(): boolean {
    return this.isDisposed;
  }
}

export class AsyncDisposableStack implements AsyncDisposable {
  private readonly resources: AsyncDisposable[];
  private isDisposed = false;

  constructor() {
    this.resources = [];
  }

  use<T extends Disposable | AsyncDisposable | null | undefined>(res: T): T {
    if (res == null) return res;

    if (Symbol.asyncDispose in res) {
      this.resources.push(res);
    } else if (Symbol.dispose in res) {
      this.resources.push({
        [Symbol.asyncDispose]: async () => {
          res[Symbol.dispose]();
        },
      });
    }

    return res;
  }

  defer(onDispose: () => Promise<void>) {
    this.resources.push({
      [Symbol.asyncDispose]: onDispose,
    });
  }

  // TODO(stevegolton): Handle error suppression properly
  // https://github.com/tc39/proposal-explicit-resource-management?tab=readme-ov-file#aggregation
  async [Symbol.asyncDispose](): Promise<void> {
    this.isDisposed = true;
    while (true) {
      const res = this.resources.pop();
      if (res === undefined) {
        break;
      }
      const timerId = setTimeout(() => {
        throw new Error(
          'asyncDispose timed out. This might be due to a Disposable ' +
            'resource  trying to issue cleanup queries on trace unload, ' +
            'while the Wasm module was already destroyed ',
        );
      }, 10_000);
      await res[Symbol.asyncDispose]();
      clearTimeout(timerId);
    }
  }

  asyncDispose(): Promise<void> {
    return this[Symbol.asyncDispose]();
  }

  adopt<T>(value: T, onDispose: (value: T) => Promise<void>): T {
    this.resources.push({
      [Symbol.asyncDispose]: async () => onDispose(value),
    });
    return value;
  }

  move(): AsyncDisposableStack {
    const other = new AsyncDisposableStack();
    for (const res of this.resources) {
      other.resources.push(res);
    }
    this.resources.length = 0;
    return other;
  }

  readonly [Symbol.toStringTag]: string = 'AsyncDisposableStack';

  get disposed(): boolean {
    return this.isDisposed;
  }
}
