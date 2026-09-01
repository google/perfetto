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

import {postMessageBus} from '../../public/post_message_bus';
import {EvtSource} from '../../base/events';

export class MsgChannelTarget {
  readonly srcWindow: Window;
  readonly srcDomain: string;
  readonly session: string;

  constructor(win: Window, domain: string, session: string) {
    this.srcWindow = win;
    this.srcDomain = domain;
    this.session = session;
  }
}

interface PostedMsgChannelTarget {
  session: string;
}

interface PostedMsgChannelTargetWrapped {
  perfetto: {
    msgchannel: PostedMsgChannelTarget;
  };
}

function isPostedMsgChannelTarget(
  obj: unknown,
): obj is PostedMsgChannelTargetWrapped {
  const wrapped = obj as PostedMsgChannelTargetWrapped;
  if (wrapped.perfetto === undefined) {
    return false;
  }

  return (
    wrapped.perfetto.msgchannel !== undefined &&
    typeof wrapped.perfetto.msgchannel.session === 'string' &&
    wrapped.perfetto.msgchannel.session.length > 0
  );
}

export class MsgChannelTargetRegistry {
  readonly providers: Array<MsgChannelTarget> = [];
  readonly onProviderRegistered = new EvtSource<MsgChannelTarget>();

  constructor() {
    // Listen for messages from the parent window to register new providers
    postMessageBus.addListener((messageEvent: MessageEvent) => {
      // Check the source of message, must come from opener or parent frame
      const fromOpener = messageEvent.source === window.opener;
      const fromIframeHost = messageEvent.source === window.parent;

      // Check if a posted message channel, if it is then add to the registry
      if (
        (fromOpener || fromIframeHost) &&
        isPostedMsgChannelTarget(messageEvent.data)
      ) {
        const postedMsgChannelTarget = messageEvent.data.perfetto.msgchannel;
        this.registerProvider(
          messageEvent.source as Window,
          messageEvent.origin,
          postedMsgChannelTarget.session,
        );
      }
    });
  }

  registerProvider(win: Window, domain: string, session: string): void {
    const provider = new MsgChannelTarget(win, domain, session);

    this.providers.push(provider);
    this.onProviderRegistered.notify(provider);
  }

  unregisterProvider(_win: Window, _domain: string, _session: string) {
    // Not implemented, as we currently have no use case for it.
    // Can be added if needed in the future.
  }
}

let sharedMsgChannelTargetRegistry: MsgChannelTargetRegistry | undefined;

export function getSharedMsgChannelTargetRegistry(): MsgChannelTargetRegistry {
  if (sharedMsgChannelTargetRegistry === undefined) {
    sharedMsgChannelTargetRegistry = new MsgChannelTargetRegistry();
  }
  return sharedMsgChannelTargetRegistry;
}
