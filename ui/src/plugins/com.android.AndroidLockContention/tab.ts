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
import {Section} from '../../widgets/section';
import {AndroidLockContentionEventSource} from './android_lock_contention_event_source';
import {Anchor} from '../../widgets/anchor';
import {Icons} from '../../base/semantic_icons';
import {time} from '../../base/time';
import {Spinner} from '../../widgets/spinner';
import {DetailsShell} from '../../widgets/details_shell';
import {Tab} from '../../public/tab';
import {
  RelatedEvent,
  RelatedEventData,
} from '../../components/related_events/interface';
import {
  TrackPinningManager,
  RelatedEventsFetcher,
} from '../../components/related_events/utils';
import {Trace} from '../../public/trace';

interface LockContentionArgs {
  short_blocked_method: string;
  blocked_thread_name: string;
  blocked_src: string;
  short_blocking_method: string;
  blocking_thread_name: string;
  blocking_src: string;
  blockingTrackUri?: string;
  blockingSliceId?: number;
  allTrackUris?: string[];
}

export class AndroidLockContentionTab implements Tab {
  private event: RelatedEvent | null = null;
  private dataFetcher: RelatedEventsFetcher;

  constructor(
    private trace: Trace,
    source: AndroidLockContentionEventSource,
    private pinningManager: TrackPinningManager,
    private onRelatedEventsLoaded?: (data: RelatedEventData) => void,
  ) {
    this.dataFetcher = new RelatedEventsFetcher((id) =>
      source.getRelatedEventData(id),
    );
  }

  // Call this method to explicitly load the lock contention event data
  load(eventId: number) {
    this.dataFetcher.load(eventId, (data) => {
      this.buildData(data);
      this.pinningManager.applyPinning(this.trace);
      if (this.onRelatedEventsLoaded) {
        this.onRelatedEventsLoaded(data);
      }
    });
  }

  private buildData(data: RelatedEventData) {
    this.event = data.events.length > 0 ? data.events[0] : null;
  }

  hasEvent(): boolean {
    return this.event !== null;
  }

  getEventArgs(): LockContentionArgs | null {
    if (this.event === null || this.event.customArgs === undefined) return null;
    return this.event.customArgs as LockContentionArgs;
  }

  getEventTrackUri(): string | undefined {
    return this.event?.trackUri;
  }

  getContentionId(): number | undefined {
    return this.event?.id;
  }

  getTitle() {
    return 'Lock Contention';
  }

  private goTo(trackUri: string, eventId: number) {
    this.trace.selection.selectTrackEvent(trackUri, eventId, {
      scrollToSelection: true,
      switchToCurrentSelectionTab: false,
    });
  }

  private scrollToTime(trackUri: string, ts: time) {
    this.trace.scrollTo({
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
    let content: m.Children;
    if (this.dataFetcher.isLoading()) {
      content = m(
        'div',
        {style: {display: 'flex', justifyContent: 'center', padding: '20px'}},
        m(Spinner, {}),
      );
    } else {
      content = this.renderContent();
    }

    return m(DetailsShell, {title: this.getTitle()}, content);
  }

  private renderContent(): m.Children {
    const event = this.event;
    if (event === null || event.customArgs === undefined) {
      return m('.note', 'Select a lock contention event.');
    }

    const args = event.customArgs as LockContentionArgs;
    const blockingTrackUri = args.blockingTrackUri;
    const blockingSliceId = args.blockingSliceId;

    return m(
      '.contention-details',
      m(
        Section,
        {
          title: 'Blocked Thread/Method',
        },
        m('div', `Thread: ${args.blocked_thread_name}`),
        m('div', `Method: ${args.short_blocked_method}`),
        m('div', `Source: ${args.blocked_src}`),
        m(
          Anchor,
          {
            icon: Icons.GoTo,
            onclick: () => this.goTo(event.trackUri, event.id),
            title: 'Go to Blocked Event',
          },
          'Go to Blocked',
        ),
      ),
      m(
        Section,
        {
          title: 'Blocking Thread/Method',
        },
        m('div', `Thread: ${args.blocking_thread_name}`),
        m('div', `Method: ${args.short_blocking_method}`),
        m('div', `Source: ${args.blocking_src}`),
        blockingTrackUri &&
          blockingSliceId !== undefined &&
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () => this.goTo(blockingTrackUri, blockingSliceId),
              title: 'Go to Blocking Slice',
            },
            'Go to Blocking Slice',
          ),
        blockingTrackUri &&
          blockingSliceId === undefined &&
          m(
            Anchor,
            {
              icon: Icons.GoTo,
              onclick: () => this.scrollToTime(blockingTrackUri, event.ts),
              title: 'Scroll to Blocking Thread at Contention Time',
            },
            'Scroll to Blocking Thread',
          ),
      ),
    );
  }
}
