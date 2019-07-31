// Copyright (C) 2019 The Android Open Source Project
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

import {globals} from './globals';

// Used to keep track of whether the engine is currently querying.
export class LoadingManager {
  private static _instance: LoadingManager;
  private currentlyLoading = 0;

  static get getInstance() {
    return this._instance || (this._instance = new this());
  }

  beginLoading() {
    this.setLoading(1);
  }

  endLoading() {
    this.setLoading(-1);
  }

  private setLoading(numberToAdd: number) {
    const previous = this.isLoading();
    this.currentlyLoading += numberToAdd;

    if (this.isLoading() !== previous) {
      globals.publish('Loading', this.isLoading());
    }
  }

  private isLoading() {
    return this.currentlyLoading > 0;
  }
}