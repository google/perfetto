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

import m from 'mithril';
import {Tab} from '../../public/tab';
import {Trace} from '../../public/trace';
import {DetailsShell} from '../../widgets/details_shell';
import {Section} from '../../widgets/section';
import {AndroidLockContentionEventSource} from './android_lock_contention_event_source';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {Spinner} from '../../widgets/spinner';
import {time} from '../../base/time';
import {RelatedEvent} from '../dev.perfetto.RelatedEvents/interface';

interface LockContentionArgs {
  short_blocked_method: string;
  short_blocking_method: string;
  blocking_thread_name: string;
  blockingTrackUri: string;
  blockingSliceId?: number;
  allTrackUris?: string[];
}

function isLockContentionArgs(args: unknown): args is LockContentionArgs {
  if (typeof args !== 'object' || args === null) return false;
  const obj = args as Record<string, unknown>;
  return (
    typeof obj.short_blocked_method === 'string' &&
    typeof obj.short_blocking_method === 'string'
  );
}

interface LockContentionTabConfig {
  trace: Trace;
  source: AndroidLockContentionEventSource;
}

export class AndroidLockContentionTab implements Tab {
  private event: RelatedEvent | null = null;
  private isLoading = false;

  constructor(private config: LockContentionTabConfig) {}

  syncSelection() {
    const selection = this.config.trace.selection.selection;
    if (selection.kind === 'track_event') {
      this.loadData(selection.eventId);
    } else {
      this.event = null;
    }
  }

  async loadData(eventId: number) {
    if (this.isLoading) return;
    this.isLoading = true;
    this.event = null;
    try {
      const data = await this.config.source.getRelatedEventData(eventId);
      this.event = data.events.length > 0 ? data.events[0] : null;
    } finally {
      this.isLoading = false;
    }
  }

  getTitle() {
    return 'Lock Contention';
  }

  private goTo(trackUri: string, eventId: number) {
    this.config.trace.selection.selectTrackEvent(trackUri, eventId, {
      scrollToSelection: true,
      switchToCurrentSelectionTab: false,
    });
  }

  private scrollToTime(trackUri: string, ts: time) {
    this.config.trace.scrollTo({
      time: {
        start: ts,
        behavior: 'pan',
      },
      track: {
        uri: trackUri,
        expandGroup: true,
      },
    });
  }

  render(): m.Children {
    if (this.isLoading) {
      return m(DetailsShell, {title: this.getTitle()}, m(Spinner, {}));
    }

    if (!this.event || !this.event.customArgs) {
      return m(
        DetailsShell,
        {title: this.getTitle()},
        m('.note', 'Select a lock contention event.'),
      );
    }

    const args = this.event.customArgs;
    if (!isLockContentionArgs(args)) {
      console.error('Invalid customArgs for LockContention event', args);
      return m(
        DetailsShell,
        {title: this.getTitle()},
        m('.note', 'Error: Invalid event data.'),
      );
    }

    return m(
      DetailsShell,
      {title: this.getTitle()},
      m(
        Section,
        {
          title: 'Contention Details',
        },
        m('div', `Blocked Method: ${args.short_blocked_method}`),
        m('div', `Blocking Method: ${args.short_blocking_method}`),
        m(
          Anchor,
          {
            icon: Icons.GoTo,
            onclick: () => this.goTo(this.event!.trackUri, this.event!.id),
            title: 'Go to Blocked Event',
          },
          'Go to Blocked',
        ),
        args.blockingTrackUri &&
          args.blockingSliceId !== undefined &&
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () =>
                this.goTo(args.blockingTrackUri!, args.blockingSliceId!),
              title: 'Go to Blocking Slice',
            },
            'Go to Blocking Slice',
          ),
        args.blockingTrackUri &&
          args.blockingSliceId === undefined &&
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () =>
                this.scrollToTime(args.blockingTrackUri!, this.event!.ts),
              title: 'Scroll to Blocking Thread at Contention Time',
            },
            'Scroll to Blocking Thread',
          ),
      ),
    );
  }
}
