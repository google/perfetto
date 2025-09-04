// Copyright (C) 2020 The Android Open Source Project
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

import m from 'mithril';
import {TraceUrlSource} from '../core/trace_source';
import {createPermalink, uploadTraceBlob} from './permalink';
import {showModal} from '../widgets/modal';
import {Trace} from '../public/trace';
import {TraceImpl} from '../core/trace_impl';
import {CopyableLink} from '../widgets/copyable_link';
import {AppImpl} from '../core/app_impl';

export function isShareable(trace: Trace) {
  return AppImpl.instance.isInternalUser && trace.traceInfo.downloadable;
}

const STATE_HASH_PLACEHOLDER = 'perfettoStateHashPlaceholder';

function urlHasPlaceholder(url: string): boolean {
  return url.includes(STATE_HASH_PLACEHOLDER);
}

export async function shareTrace(trace: TraceImpl) {
  const traceSource = trace.traceInfo.source;
  const traceUrl = (traceSource as TraceUrlSource).url ?? '';
  const hasPlaceholder = urlHasPlaceholder(traceUrl);

  if (isShareable(trace)) {
    // Just upload the trace and create a permalink.
    const result = confirm(
      `Upload UI state and generate a permalink? ` +
        `The trace will be accessible by anybody with the permalink.`,
    );

    if (result) {
      const traceUrl = await uploadTraceBlob(trace);
      const hash = await createPermalink(trace, traceUrl);
      showModal({
        title: 'Permalink',
        content: m(CopyableLink, {
          url: `${self.location.origin}/#!/?s=${hash}`,
        }),
      });
    }
  } else {
    if (traceUrl) {
      if (hasPlaceholder) {
        // Trace is not sharable, but has a URL and a placeholder. Upload the
        // state and return the URL with the placeholder filled in.
        // Trace is not sharable, but has a URL with no placeholder.
        // Just upload the trace and create a permalink.
        const result = confirm(
          `Upload UI state and generate a permalink? ` +
            `The state (not the trace) will be accessible by anybody with the permalink.`,
        );

        if (result) {
          const hash = await createPermalink(trace, undefined);
          const urlWithHash = traceUrl.replace(STATE_HASH_PLACEHOLDER, hash);
          showModal({
            title: 'Permalink',
            content: m(CopyableLink, {url: urlWithHash}),
          });
        }
      } else {
        // Trace is not sharable, has a URL, but no placeholder.
        showModal({
          title: 'Cannot create permalink from external trace',
          content: m(
            '',
            m(
              'p',
              'This trace was opened by an external site and as such cannot ' +
                'be re-shared preserving the UI state. ',
            ),
            m('p', 'By using the URL below you can open this trace again.'),
            m('p', 'Clicking will copy the URL into the clipboard.'),
            m(CopyableLink, {url: traceUrl}),
          ),
        });
      }
    } else {
      // Trace is not sharable and has no URL. Nothing we can do. Just tell the
      // user.
      showModal({
        title: 'Cannot create permalink',
        content: m(
          'p',
          'This trace was opened by an external site and as such cannot ' +
            'be re-shared preserving the UI state. ',
        ),
      });
    }
  }
}
