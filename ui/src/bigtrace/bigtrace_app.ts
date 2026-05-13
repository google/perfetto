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

import {CommandManagerImpl} from '../core/command_manager';
import {OmniboxManagerImpl} from '../core/omnibox_manager';

// Lightweight singleton for the BigTrace app. Holds only the managers needed
// for the omnibox and command palette — no WASM, no trace processor.
export class BigTraceApp {
  readonly omnibox: OmniboxManagerImpl;
  readonly commands: CommandManagerImpl;

  private constructor() {
    this.omnibox = new OmniboxManagerImpl();
    this.commands = new CommandManagerImpl(this.omnibox);
  }

  private static _instance?: BigTraceApp;

  static get instance(): BigTraceApp {
    if (!BigTraceApp._instance) {
      BigTraceApp._instance = new BigTraceApp();
    }
    return BigTraceApp._instance;
  }
}
