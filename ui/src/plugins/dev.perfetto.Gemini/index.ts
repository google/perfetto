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

import type {PerfettoPlugin} from '../../public/plugin';
import IntellettoPlugin from '../dev.perfetto.Intelletto';
import {geminiProtocol} from './gemini';

export default class implements PerfettoPlugin {
  static readonly id = 'dev.perfetto.Gemini';
  static readonly dependencies = [IntellettoPlugin];
  static readonly description = 'Gemini protocol for Intelletto plugin.';

  static onActivate(): void {
    // Built-in protocols.
    IntellettoPlugin.registerProtocol(geminiProtocol);
  }
}
