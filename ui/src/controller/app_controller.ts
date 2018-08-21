// Copyright (C) 2018 The Android Open Source Project
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

import {globals} from '../controller/globals';
import {Child, Controller, ControllerInitializerAny} from './controller';
import {PermalinkController} from './permalink_controller';
import {TraceController} from './trace_controller';

// The root controller for the entire app. It handles the lifetime of all
// the other controllers (e.g., track and query controllers) according to the
// global state.
export class AppController extends Controller<'main'> {
  constructor() {
    super('main');
  }

  // This is the root method that is called every time the controller tree is
  // re-triggered. This can happen due to:
  // - An action received from the frontend.
  // - An internal promise of a nested controller being resolved and manually
  //   re-triggering the controllers.
  run() {
    const engineKeys = Object.keys(globals.state.engines);
    const childControllers: ControllerInitializerAny[] = [
      Child('permalink', PermalinkController, {}),
    ];
    if (engineKeys.length > 0) {
      const cfg = globals.state.engines[engineKeys[0]];
      childControllers.push(Child(cfg.id, TraceController, cfg.id));
    }
    return childControllers;
  }
}
