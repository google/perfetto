// Copyright 2026 Comcast Cable Communications Management, LLC
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

import {EvtSource, type Evt} from '../base/events';

/**
 * Public pub/sub surface that bridges window `postMessage` events into plugin
 * code without breaking the import rules enforced by tools/check_imports.
 *
 * The frontend's `post_message_handler.ts` lives in `/frontend/*`, which
 * plugins are not allowed to import from. Instead, the handler imports this
 * module (frontend -> public is allowed) and calls `notifyPostMessageBus()`.
 * Plugins import `postMessageBus` from `/public/*` (which is allowed) and
 * subscribe via `addListener()`.
 *
 * Listeners receive the raw `MessageEvent` _after_ the built-in handler has
 * had a chance to process it (PING/PONG, trace loading, scroll-to-range,
 * etc). The bus does NOT fire for messages that the built-in handler chose
 * to ignore early (e.g. `perfettoIgnore === true`, untrusted sources, or
 * messages received before `document.readyState === 'complete'`).
 *
 * Example (from inside a plugin):
 *
 *   import {postMessageBus} from '../../public/post_message_bus';
 *
 *   onActivate(app: App) {
 *     app.trash.use(postMessageBus.addListener((ev) => {
 *       if (ev.data?.perfetto?.type === 'rdk.recordTrace') {
 *         // handle plugin-specific message
 *       }
 *     }));
 *   }
 */
class PostMessageBusImpl extends EvtSource<MessageEvent> {}

const postMessageBusImpl = new PostMessageBusImpl();

export const postMessageBus: Evt<MessageEvent> = postMessageBusImpl;

// For use by /frontend/post_message_handler.ts.
export function notifyPostMessageBus(ev: MessageEvent): Promise<void> {
  return postMessageBusImpl.notify(ev);
}
