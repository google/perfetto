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

import './native.scss';
import m from 'mithril';
import {classNames} from '../../../../base/classnames';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Icon} from '../../../../widgets/icon';
import {Panel} from '../../components/panel';
import {formatBytes, formatDelta} from '../landing_page/mem_format';

export interface MemoryTriageSummary {
  readonly nativeSmapsBytes: number;
  readonly javaSmapsBytes: number;
  readonly graphicsSmapsBytes: number;
  readonly fileSmapsBytes: number;
  readonly nativeDeltaBytes?: number;
  readonly javaDeltaBytes?: number;
  readonly heapprofdUnreleasedBytes: number;
  readonly javaHeapBytes: number;
  readonly javaRegisteredNativeBytes: number;
  readonly hasSmaps: boolean;
  readonly hasNativeProfile: boolean;
  readonly hasJavaDump: boolean;
}

export interface MemoryDomainGuideAttrs {
  readonly triage?: MemoryTriageSummary;
  readonly onOpenNative: () => void;
  readonly onOpenJava: () => void;
  readonly onOpenSmaps?: () => void;
  readonly onOpenTimeline?: () => void;
}

const CHEAT_CARDS = [
  {
    color: '#34a853',
    title: 'Overview & Smaps',
    tag: 'Physical ground truth',
    desc: 'Every mapped page (RSS + Swap) across Native, Java/ART, .so/.oat code, Graphics/DMA-BUF, and thread stacks.',
    best: 'Confirm which bucket is large or growing over time.',
    blind: 'No callstacks or object reference chains.',
  },
  {
    color: '#4285f4',
    title: 'Native Memory',
    tag: 'heapprofd',
    desc: 'Sampled malloc / new / free callstacks separating retained leaks from allocation churn.',
    best: 'C++/NDK, JNI, Skia, WebView, Media, libc/scudo growth & churn.',
    blind: 'Allocations before profiling started and raw mmap / GPU buffers.',
  },
  {
    color: '#f4b400',
    title: 'Java Memory',
    tag: 'java_hprof',
    desc: 'Point-in-time snapshot of live Java/Kotlin objects, dominator trees, and GC root chains.',
    best: 'Activity/Fragment leaks, caches, duplicate Strings/Arrays, Bitmaps.',
    blind: 'Unregistered C++/JNI malloc (sees only NativeAllocationRegistry).',
  },
] as const;

export class MemoryDomainGuide implements m.ClassComponent<MemoryDomainGuideAttrs> {
  private guideOpen = false;

  view({attrs}: m.CVnode<MemoryDomainGuideAttrs>): m.Children {
    const {triage, onOpenNative, onOpenJava, onOpenSmaps, onOpenTimeline} =
      attrs;
    const rec = triage ? buildRecommendation(triage) : undefined;
    return m(
      Panel,
      {
        className: classNames(
          'pf-memscope-guide',
          this.guideOpen && 'pf-memscope-guide--open',
        ),
      },
      m('.pf-memscope-guide__top', [
        m('.pf-memscope-guide__left', [
          m('.pf-memscope-guide__kicker', [
            m(Icon, {icon: 'explore'}),
            m('span', 'Where to look first'),
          ]),
          rec
            ? m('.pf-memscope-guide__rec', [
                m('span.pf-memscope-guide__dot', {
                  style: {background: rec.color},
                }),
                m('span', rec.text),
              ])
            : m(
                '.pf-memscope-guide__rec',
                'Open Native (heapprofd) or Java (java_hprof) below to inspect memory in detail.',
              ),
        ]),
        m('.pf-memscope-guide__actions', [
          triage?.hasNativeProfile &&
            m(Button, {
              label: 'Native memory',
              icon: 'memory',
              rightIcon: 'arrow_forward',
              variant:
                rec?.domain === 'native'
                  ? ButtonVariant.Filled
                  : ButtonVariant.Outlined,
              compact: true,
              onclick: onOpenNative,
            }),
          triage?.hasJavaDump &&
            m(Button, {
              label: 'Java heap',
              icon: 'data_object',
              rightIcon: 'arrow_forward',
              variant:
                rec?.domain === 'java'
                  ? ButtonVariant.Filled
                  : ButtonVariant.Outlined,
              compact: true,
              onclick: onOpenJava,
            }),
          onOpenSmaps &&
            triage?.hasSmaps &&
            rec?.domain === 'smaps' &&
            m(Button, {
              label: 'Smaps detail',
              rightIcon: 'arrow_forward',
              variant: ButtonVariant.Filled,
              compact: true,
              onclick: onOpenSmaps,
            }),
          onOpenTimeline &&
            m(Button, {
              label: 'Focused timeline',
              icon: 'view_timeline',
              variant: ButtonVariant.Minimal,
              compact: true,
              onclick: onOpenTimeline,
            }),
          m(Button, {
            label: this.guideOpen ? 'Hide guide' : 'Native vs Java guide',
            icon: this.guideOpen ? 'expand_less' : 'help_outline',
            variant: ButtonVariant.Minimal,
            compact: true,
            onclick: () => {
              this.guideOpen = !this.guideOpen;
            },
          }),
        ]),
      ]),
      m(
        '.pf-memscope-guide__slide',
        m(
          '.pf-memscope-guide__inner',
          this.guideOpen &&
            m('.pf-memscope-guide__sheet', [
              m(
                '.pf-memscope-guide__cols',
                CHEAT_CARDS.map((c) =>
                  m(
                    '.pf-memscope-guide__card',
                    {style: {'--accent': c.color}},
                    [
                      m('.pf-memscope-guide__card-title', [
                        m('span.pf-memscope-guide__dot', {
                          style: {background: c.color},
                        }),
                        c.title,
                        m('span.pf-memscope-guide__tag', c.tag),
                      ]),
                      m('p', c.desc),
                      m('ul', [
                        m('li', [m('strong', 'Best for: '), c.best]),
                        m('li', [m('strong', 'Blind spots: '), c.blind]),
                      ]),
                    ],
                  ),
                ),
              ),
              m('.pf-memscope-guide__bridge', [
                m(Icon, {icon: 'compare_arrows'}),
                m('span', [
                  m('strong', 'Cross-boundary: '),
                  'Android ',
                  m('code', 'Bitmap'),
                  ' / direct ',
                  m('code', 'ByteBuffer'),
                  ' buffers live in native/graphics memory but are retained by Java wrappers — check ',
                  m('strong', 'Java → Bitmaps / Dominators'),
                  ' for retainers and ',
                  m('strong', 'Native → Flamegraph'),
                  ' for pure C++/JNI leaks.',
                ]),
              ]),
            ]),
        ),
      ),
    );
  }
}

function buildRecommendation(t: MemoryTriageSummary): {
  readonly domain: 'native' | 'java' | 'smaps';
  readonly color: string;
  readonly text: m.Children;
} {
  const nDelta = t.nativeDeltaBytes ?? 0;
  const jDelta = t.javaDeltaBytes ?? 0;
  if (nDelta > 512 * 1024 && nDelta > jDelta * 1.5) {
    return {
      domain: 'native',
      color: '#4285f4',
      text: [
        m('strong', 'Native memory'),
        ` drove most growth (${formatDelta(nDelta)}). `,
        t.hasNativeProfile
          ? 'Open Native → Flamegraph or Growth & Churn for allocating callstacks.'
          : 'Record with heapprofd to see allocating callstacks.',
      ],
    };
  }
  if (jDelta > 512 * 1024 && jDelta > nDelta * 1.5) {
    return {
      domain: 'java',
      color: '#f4b400',
      text: [
        m('strong', 'Java heap'),
        ` drove most growth (${formatDelta(jDelta)}). `,
        t.hasJavaDump
          ? 'Open Java → Dominators or Classes to trace retaining GC roots.'
          : 'Capture a java_hprof dump to inspect retaining objects.',
      ],
    };
  }
  if (
    t.graphicsSmapsBytes > 0 &&
    t.graphicsSmapsBytes > t.nativeSmapsBytes &&
    t.graphicsSmapsBytes > t.javaSmapsBytes
  ) {
    return {
      domain: 'smaps',
      color: '#a142f4',
      text: [
        m(
          'strong',
          `Graphics / DMA-BUF (${formatBytes(t.graphicsSmapsBytes)})`,
        ),
        ' is the largest bucket — inspect Graphics, Smaps, and the Focused Timeline.',
      ],
    };
  }
  if (t.hasNativeProfile && !t.hasJavaDump) {
    return {
      domain: 'native',
      color: '#4285f4',
      text: [
        m(
          'strong',
          `Native heap profile (${formatBytes(t.heapprofdUnreleasedBytes)} unreleased)`,
        ),
        ' is available. Open Native for callstacks, .so libraries, and churn.',
      ],
    };
  }
  if (t.hasJavaDump && !t.hasNativeProfile) {
    const regNote =
      t.javaRegisteredNativeBytes > 256 * 1024
        ? `, including ${formatBytes(t.javaRegisteredNativeBytes)} registered native`
        : '';
    return {
      domain: 'java',
      color: '#f4b400',
      text: [
        m(
          'strong',
          `Java heap dump (${formatBytes(t.javaHeapBytes)}${regNote})`,
        ),
        ' is available. Open Java → Dominators, Classes, or Bitmaps.',
      ],
    };
  }
  if (t.nativeSmapsBytes > t.javaSmapsBytes && t.nativeSmapsBytes > 0) {
    return {
      domain: 'native',
      color: '#4285f4',
      text: [
        m('strong', `Native (${formatBytes(t.nativeSmapsBytes)})`),
        ` outweighs Java (${formatBytes(t.javaSmapsBytes)}) in smaps. Start in Native, or check Java if registered native is high.`,
      ],
    };
  }
  return {
    domain: 'java',
    color: '#f4b400',
    text: [
      m('strong', `Java (${formatBytes(t.javaSmapsBytes || t.javaHeapBytes)})`),
      ' is the primary managed footprint. Start in Java → Dominators & Classes.',
    ],
  };
}

export function renderDomainEmptyState(opts: {
  readonly domain: 'native' | 'java';
  readonly smapsBytes?: number;
  readonly processName: string;
  readonly onBackToOverview: () => void;
}): m.Children {
  const isNative = opts.domain === 'native';
  return m('.pf-memscope-domain-empty', [
    m('.pf-memscope-domain-empty__card', [
      m('.pf-memscope-domain-empty__head', [
        m('span.pf-memscope-guide__dot', {
          style: {background: isNative ? '#4285f4' : '#f4b400'},
        }),
        m(
          'h2',
          isNative
            ? 'No native heap profile (heapprofd) in this trace'
            : 'No Java heap dump (java_hprof) in this trace',
        ),
      ]),
      opts.smapsBytes !== undefined &&
        opts.smapsBytes > 0 &&
        m(
          'p.pf-memscope-domain-empty__smaps',
          `Smaps shows ${formatBytes(opts.smapsBytes)} of ${isNative ? 'native allocator' : 'Java/ART'} memory for ${opts.processName}, but ${isNative ? 'heapprofd' : 'java_hprof'} was not recorded.`,
        ),
      m(
        'p',
        isNative
          ? 'Native memory views require android.heapprofd for callstacks, .so attribution, and churn.'
          : 'Java memory views require android.java_hprof for objects, dominators, Bitmaps, and GC roots.',
      ),
      m(
        'code.pf-memscope-domain-empty__cmd',
        `memscope record --app ${opts.processName}`,
      ),
      m(
        '.pf-memscope-domain-empty__actions',
        m(Button, {
          label: 'Back to Overview',
          icon: 'arrow_back',
          variant: ButtonVariant.Outlined,
          onclick: opts.onBackToOverview,
        }),
      ),
    ]),
  ]);
}
