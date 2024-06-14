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

// An (almost) ESnext spec-compliant shim/polyfill/replacement for:
// - Disposable
// - AsyncDisposable
// - DisposableStack
// - AsyncDisposableStack
//
// Typescript 5.2 introduces support for ESnext's explicit resource management,
// but upgrading to it is not straightforward. However, because we still want to
// take advantage of disposables, this file acts as a shim, implementing the
// basics of disposables, which can be used in the meantime.
//
// See:
// - https://github.com/tc39/proposal-explicit-resource-management
// - https://www.typescriptlang.org/docs/handbook/release-notes/typescript-5-2.html

// Represents an object that can/should be disposed of to release resources or
// perform cleanup operations.
export interface Disposable {
  dispose(): void;
}

export interface AsyncDisposable {
  disposeAsync(): Promise<void>;
}

// A collection of Disposables.
// Disposables can be added one by one, (e.g. during the lifecycle of a
// component) then can all be disposed at once (e.g. when the component
// is destroyed). Resources are disposed LIFO.
export class DisposableStack implements Disposable {
  private resources: Disposable[];

  constructor() {
    this.resources = [];
  }

  use(d: Disposable) {
    this.resources.push(d);
  }

  defer(onDispose: () => void) {
    this.use({
      dispose: onDispose,
    });
  }

  dispose() {
    while (true) {
      const d = this.resources.pop();
      if (d === undefined) {
        break;
      }
      d.dispose();
    }
  }
}

export class AsyncDisposableStack implements AsyncDisposable {
  private resources: AsyncDisposable[] = [];

  use(d: AsyncDisposable) {
    this.resources.push(d);
  }

  defer(onDispose: () => Promise<void>) {
    this.use({
      disposeAsync: onDispose,
    });
  }

  async disposeAsync(): Promise<void> {
    while (true) {
      const d = this.resources.pop();
      if (d === undefined) {
        break;
      }
      await d.disposeAsync();
    }
  }
}
