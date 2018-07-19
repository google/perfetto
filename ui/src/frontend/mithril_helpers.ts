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

import {Action} from '../common/actions';
import {globals} from './globals';

/**
 * Events triggered by Mithril have an adittional property: redraw.
 * Redraw is a boolean which controls whether Mithril schedules an automatic
 * redraw after this event is handled.
 */
export interface MithrilEvent extends Event { redraw: boolean; }

/**
 * Create a Mithril event handler which (when triggered) dispatches an action
 * to the controller thread without causing a Mithril redraw.
 * This prevents redrawing twice for every action (once immediately and once
 * when the state updates). This function is overloaded, it can either create a
 * handler which ignores the event entirely (via the (e: Event) overload) or
 * compute the Action from the Event (via the ((e: Event) => Action) overload.
 */
export function quietDispatch(action: ((e: Event) => Action)|
                              Action): (e: Event) => void {
  if (action instanceof Function) {
    return quietHandler(event => globals.dispatch(action(event)));
  }
  return quietHandler(_ => globals.dispatch(action));
}

/**
 * Create a Mithril event handler which does not schedule a Mithril redraw.
 */
export function quietHandler(handler: (event: Event) => void) {
  return (event: Event) => {
    (event as MithrilEvent).redraw = false;
    handler(event);
  };
}
