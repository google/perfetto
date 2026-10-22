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

// The HUD, in one of two layouts. Row: a full-width row, e.g. above the
// topbar.
//
//   [● LIVE STREAM] [worker/3 ▾]      [Follow Live: ON [Space]] [Pause] [⋯]
//     │               │                                              │
//   status;         target menu:                  Reconnect, Download Trace,
//   popout below    filter or <name>/<index>      Poll Interval, Buffer
//   on errors and                                 Window, History
//   buffering
//
// Panel: a side panel tab, with the ⋯ menu's items in the open.
//
//   [● LIVE STREAM] [worker/3          ▾]   status and target
//   [popout on errors and buffering    ×]
//   ─────────────────────────────────────
//   [      Follow Live: ON [Space]      ]   actions
//   [Pause] [↻ Reconnect] [⬇ Download Trace]
//   ─────────────────────────────────────
//   SETTINGS                                settings
//   Poll Interval                [0.25s ▾]
//   Buffer Window                [5s    ▾]
//   History                      [1h    ▾]
//
// Embedding pages may style the root class .pf-live-stream-hud, e.g. through
// the CSS variables in styles.scss, and read the status from the badge's
// class, e.g. .pf-live-stream-hud__badge--error: keep these classes stable.

import m from 'mithril';
import {classNames} from '../../base/classnames';
import {DisposableStack} from '../../base/disposable_stack';
import {clamp} from '../../base/math_utils';
import {Icon} from '../../widgets/icon';
import {
  BUFFER_DELAYS,
  type LiveSettings,
  RETENTIONS,
  UPDATE_INTERVALS,
} from './settings';
import type {LiveStatus} from './status';
import {
  filterTargets,
  hasExactTarget,
  isIndexed,
  resolveTargetSpec,
  targetBaseName,
  targetDescription,
  targetLabel,
  targetLink,
  targetShortName,
} from './target_spec';
import type {Target} from './wire';

// Statuses other than live and paused show once they last this long, so that
// short hiccups don't flicker.
const STATUS_DELAY_MS = 1_000;

/** Where the HUD shows: a full-width row, or a side panel tab. */
export type LiveHudLayout = 'row' | 'panel';

/** What the HUD shows, and what its controls do. */
export interface LiveHudProps {
  readonly layout: LiveHudLayout;
  // The displayed status, see LiveHudState.syncStatus().
  readonly status: LiveStatus;
  // Shown in the popout of the error status.
  readonly error: string;
  // The server's targets. The target menu shows once they load.
  readonly targets: readonly Target[];
  // The open target, or the one being opened.
  readonly target?: Target;
  readonly following: boolean;
  readonly paused: boolean;
  readonly settings: LiveSettings;
  readonly hudState: LiveHudState;
  // Opens the target of server spec `spec`.
  onOpenTarget(spec: string): void;
  // Called when the target menu opens, e.g. to reload the targets.
  onTargetMenuOpen(): void;
  onToggleFollow(): void;
  onTogglePause(): void;
  onReconnect(): void;
  onDownload(): void;
  // Called when the user dismisses the popout of error `error`.
  onDismissError(error: string): void;
}

/**
 * State of the HUD that outlives its renders: the open menus, the target
 * search, the dismissed popout and the displayed status.
 */
export class LiveHudState implements Disposable {
  isTargetMenuOpen = false;
  targetSearchInput = '';
  // Whether the user edited the search, which then filters the targets.
  targetSearchDirty = false;
  // The listed target picked with the arrow keys, which Enter opens.
  highlightedTarget?: string;
  isMoreMenuOpen = false;
  // The popout that the user dismissed, if still shown.
  dismissedPopoutKey = '';
  // Undefined until the first status, which shows at once.
  private displayedStatus?: LiveStatus;
  private pendingStatus?: LiveStatus;
  private statusTimer?: ReturnType<typeof setTimeout>;
  // Removes the listeners of listen().
  private readonly listeners = new DisposableStack();

  constructor(private readonly redraw: () => void) {}

  /**
   * Returns the status to display for the raw status of `getRawStatus`, and
   * whether it changed. Live and paused show at once, others once they last
   * STATUS_DELAY_MS. A new displayed status shows a dismissed popout again.
   */
  syncStatus(getRawStatus: () => LiveStatus): {
    readonly status: LiveStatus;
    readonly changed: boolean;
  } {
    const raw = getRawStatus();
    const shown = this.displayedStatus;
    if (
      shown === undefined ||
      shown === raw ||
      raw === 'live' ||
      raw === 'paused'
    ) {
      this.clearPendingStatus();
      if (shown === raw) return {status: raw, changed: false};
      this.show(raw);
      return {status: raw, changed: true};
    }

    if (this.pendingStatus !== raw) {
      this.clearPendingStatus();
      this.pendingStatus = raw;
      this.statusTimer = setTimeout(() => {
        this.statusTimer = undefined;
        this.pendingStatus = undefined;
        const current = getRawStatus();
        if (current === raw && this.displayedStatus !== current) {
          this.show(current);
          this.redraw();
        }
      }, STATUS_DELAY_MS);
    }
    return {status: shown, changed: false};
  }

  /** Closes the menus on clicks outside them and on Escape, until disposed. */
  listen(): void {
    const onMouseDown = (e: MouseEvent) => {
      if (!this.isTargetMenuOpen && !this.isMoreMenuOpen) return;
      const target = e.target instanceof Element ? e.target : undefined;
      let changed = false;
      if (this.isTargetMenuOpen && !isWithin(target, TARGET_DROPDOWN)) {
        this.isTargetMenuOpen = false;
        changed = true;
      }
      if (this.isMoreMenuOpen && !isWithin(target, MORE_DROPDOWN)) {
        this.isMoreMenuOpen = false;
        changed = true;
      }
      if (changed) this.redraw();
    };
    // An Escape that closes a menu isn't for the timeline, e.g. to deselect.
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key !== 'Escape' || !this.closeMenus()) return;
      e.stopPropagation();
      this.redraw();
    };
    window.addEventListener('mousedown', onMouseDown);
    window.addEventListener('keydown', onKeyDown, true);
    this.listeners.defer(() => {
      window.removeEventListener('mousedown', onMouseDown);
      window.removeEventListener('keydown', onKeyDown, true);
    });
  }

  /** Closes the menus. Returns whether one was open. */
  closeMenus(): boolean {
    const wasOpen = this.isTargetMenuOpen || this.isMoreMenuOpen;
    this.isTargetMenuOpen = false;
    this.isMoreMenuOpen = false;
    return wasOpen;
  }

  [Symbol.dispose](): void {
    this.clearPendingStatus();
    this.listeners[Symbol.dispose]();
  }

  private show(status: LiveStatus) {
    this.displayedStatus = status;
    this.dismissedPopoutKey = '';
  }

  private clearPendingStatus() {
    clearTimeout(this.statusTimer);
    this.statusTimer = undefined;
    this.pendingStatus = undefined;
  }
}

const TARGET_DROPDOWN = '.pf-live-stream-hud__target-dropdown';
const MORE_DROPDOWN = '.pf-live-stream-hud__more-dropdown';

function isWithin(element: Element | undefined, selector: string): boolean {
  return (element?.closest(selector) ?? null) !== null;
}

const BADGES: Record<LiveStatus, {readonly label: string}> = {
  live: {label: 'LIVE STREAM'},
  buffering: {label: 'BUFFERING'},
  paused: {label: 'PAUSED'},
  error: {label: 'STREAM ERROR'},
};

/** Renders the HUD in `props.layout`. */
export function renderLiveStreamHud(props: LiveHudProps): m.Children {
  return props.layout === 'panel' ? renderPanel(props) : renderRow(props);
}

/** What the panel shows while a trace that isn't live is open. */
export function renderOtherTraceNote(): m.Children {
  return m(
    '.pf-live-stream-hud.pf-live-stream-hud--panel',
    m(
      '.pf-live-stream-hud__note',
      'Live stream controls are off while another trace is open.',
    ),
  );
}

// Status and target on the left, actions on the right, the rest in ⋯.
function renderRow(props: LiveHudProps): m.Children {
  const {badge, popout} = renderStatus(props);
  return m(
    '.pf-live-stream-hud.pf-live-stream-hud--row',
    m(
      '.pf-live-stream-hud__row.pf-live-stream-hud__row--left',
      m('.pf-live-stream-hud__status-wrap', badge, popout),
      props.targets.length > 0 && renderTargetDropdown(props),
    ),
    m(
      '.pf-live-stream-hud__row.pf-live-stream-hud__row--right',
      renderFollowButton(props),
      renderPauseButton(props),
      renderMoreDropdown(props),
    ),
  );
}

// Status and target, then all actions, then the settings. The popout shows
// in line, so that it covers no control.
function renderPanel(props: LiveHudProps): m.Children {
  const {badge, popout} = renderStatus(props);
  return m(
    '.pf-live-stream-hud.pf-live-stream-hud--panel',
    m(
      '.pf-live-stream-hud__section',
      m(
        '.pf-live-stream-hud__status-line',
        badge,
        props.targets.length > 0 && renderTargetDropdown(props),
      ),
      popout,
    ),
    m(
      '.pf-live-stream-hud__section.pf-live-stream-hud__actions',
      renderFollowButton(props),
      renderPauseButton(props),
      moreActions(props).map((action) =>
        m(
          'button.pf-live-stream-hud__btn',
          {
            title: action.title,
            onclick: (e: MouseEvent) => {
              blurTarget(e);
              action.run();
            },
          },
          m('span.pf-live-stream-hud__btn-icon', action.icon),
          action.label,
        ),
      ),
    ),
    m(
      '.pf-live-stream-hud__section',
      m('.pf-live-stream-hud__section-title', 'Settings'),
      renderSettings(props.settings),
    ),
  );
}

function renderFollowButton(props: LiveHudProps): m.Children {
  return m(
    'button.pf-live-stream-hud__btn.pf-live-stream-hud__btn--follow',
    {
      className: classNames(
        props.following && 'pf-live-stream-hud__btn--active',
      ),
      title: 'Toggle live auto-scroll (Shortcut: Space)',
      onclick: (e: MouseEvent) => {
        blurTarget(e);
        props.onToggleFollow();
      },
    },
    props.following ? 'Follow Live: ON [Space]' : 'Follow Live: OFF [Space]',
  );
}

function renderPauseButton(props: LiveHudProps): m.Children {
  return m(
    'button.pf-live-stream-hud__btn',
    {
      className: classNames(props.paused && 'pf-live-stream-hud__btn--active'),
      onclick: (e: MouseEvent) => {
        blurTarget(e);
        props.onTogglePause();
      },
    },
    props.paused ? 'Resume Live' : 'Pause',
  );
}

// The status badge, and the popout under it for errors and buffering until
// dismissed, apart so that each layout places them. A new text, a click on
// the badge, or Enter shows a dismissed popout again; Space stays the Follow
// hotkey.
function renderStatus(props: LiveHudProps): {
  readonly badge: m.Children;
  readonly popout: m.Children;
} {
  const {status, hudState} = props;
  const popoutKey =
    status === 'error'
      ? `error:${props.error}`
      : status === 'buffering'
        ? 'buffering'
        : '';
  const dismissed =
    popoutKey !== '' && hudState.dismissedPopoutKey === popoutKey;
  const pulse = status === 'live' || status === 'buffering';
  const showPopout = () => {
    if (dismissed) hudState.dismissedPopoutKey = '';
  };

  const badge = m(
    `.pf-live-stream-hud__badge.pf-live-stream-hud__badge--${status}`,
    {
      role: 'button',
      tabindex: 0,
      title: dismissed ? 'Click to show stream status details' : undefined,
      onclick: showPopout,
      // Not for the timeline's hotkeys, e.g. Enter for the next search
      // result.
      onkeydown: (e: KeyboardEvent) => {
        if (e.key !== 'Enter') return;
        e.stopPropagation();
        showPopout();
      },
    },
    m('.pf-live-stream-hud__dot', {
      className: classNames(pulse && 'pf-live-stream-hud__dot--pulse'),
    }),
    BADGES[status].label,
  );
  const popout =
    popoutKey !== '' &&
    !dismissed &&
    m(
      `.pf-live-stream-hud__popout.pf-live-stream-hud__popout--${status}`,
      m('.pf-live-stream-hud__popout-arrow'),
      m(
        '.pf-live-stream-hud__popout-body',
        m(
          `.pf-live-stream-hud__popout-title` +
            `.pf-live-stream-hud__popout-title--${status}`,
          status === 'error' ? 'Stream Error' : 'Buffering',
        ),
        m(
          '.pf-live-stream-hud__popout-msg',
          status === 'error' ? props.error : 'Buffering live stream...',
        ),
      ),
      m(
        'button.pf-live-stream-hud__popout-close',
        {
          'title': 'Dismiss',
          'aria-label': 'Dismiss',
          'onclick': (e: MouseEvent) => {
            e.stopPropagation();
            blurTarget(e);
            hudState.dismissedPopoutKey = popoutKey;
            if (status === 'error') props.onDismissError(props.error);
          },
        },
        '×',
      ),
    );
  return {badge, popout};
}

function renderTargetDropdown(props: LiveHudProps): m.Children {
  const {hudState, target} = props;
  const name = target === undefined ? '' : targetShortName(target);
  return m(
    '.pf-live-stream-hud__target-dropdown',
    m(
      'button.pf-live-stream-hud__target-trigger',
      {
        title: target === undefined ? undefined : targetLabel(target),
        onclick: (e: MouseEvent) => {
          blurTarget(e);
          hudState.isTargetMenuOpen = !hudState.isTargetMenuOpen;
          if (!hudState.isTargetMenuOpen) return;
          hudState.isMoreMenuOpen = false;
          // Prefilled, so that e.g. another task is a backspace away.
          hudState.targetSearchInput = name;
          hudState.targetSearchDirty = false;
          hudState.highlightedTarget = undefined;
          props.onTargetMenuOpen();
        },
      },
      m('span.pf-live-stream-hud__target-trigger-label', name),
      m(
        'span.pf-live-stream-hud__target-trigger-caret',
        hudState.isTargetMenuOpen ? '▴' : '▾',
      ),
    ),
    hudState.isTargetMenuOpen && renderTargetMenu(props),
  );
}

// The search box filters the targets once edited; Enter or Go opens the typed
// target, e.g. "<name>/<index>". ↑/↓ highlight a listed target, which Enter
// then opens instead.
function renderTargetMenu(props: LiveHudProps): m.Children {
  const {hudState, targets, target} = props;
  const query = hudState.targetSearchInput.trim();
  const shown = listedTargets(props);
  const spec = resolveTargetSpec(targets, query);
  const activeBase = target === undefined ? '' : targetBaseName(target);
  const highlighted = shown.findIndex(
    (t) => t.id === hudState.highlightedTarget,
  );
  const submit = () => {
    const typed = resolveTargetSpec(targets, hudState.targetSearchInput);
    if (typed === undefined) return;
    hudState.isTargetMenuOpen = false;
    props.onOpenTarget(typed);
  };

  return m(
    '.pf-live-stream-hud__target-menu',
    m(
      '.pf-live-stream-hud__target-search-row',
      m('input.pf-live-stream-hud__target-search-input', {
        'type': 'text',
        'role': 'combobox',
        'aria-label': 'Filter targets',
        'aria-expanded': 'true',
        'aria-controls': TARGET_LIST_ID,
        'aria-activedescendant':
          highlighted < 0 ? undefined : targetOptionId(highlighted),
        'value': hudState.targetSearchInput,
        'placeholder': targets.some(isIndexed)
          ? 'Filter or enter <name>/<index>...'
          : 'Filter targets...',
        'spellcheck': false,
        'autocomplete': 'off',
        'oncreate': (vnode: m.VnodeDOM) => {
          const input = vnode.dom;
          if (!(input instanceof HTMLInputElement)) return;
          input.focus();
          input.setSelectionRange(input.value.length, input.value.length);
        },
        'oninput': (e: InputEvent) => {
          if (!(e.target instanceof HTMLInputElement)) return;
          hudState.targetSearchInput = e.target.value;
          hudState.targetSearchDirty = true;
          hudState.highlightedTarget = undefined;
        },
        'onkeydown': (e: KeyboardEvent) => {
          // Typing is not for the timeline's hotkeys.
          e.stopPropagation();
          if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
            e.preventDefault();
            highlightTarget(props, e.key === 'ArrowDown' ? 1 : -1);
          } else if (e.key === 'Escape') {
            hudState.isTargetMenuOpen = false;
          } else if (e.key === 'Enter') {
            e.preventDefault();
            // Unless a list refresh removed the highlighted target.
            const id = hudState.highlightedTarget;
            if (id !== undefined && shown.some((t) => t.id === id)) {
              openListedTarget(props, id);
            } else {
              submit();
            }
          }
        },
      }),
      m(
        'button.pf-live-stream-hud__target-search-go',
        {
          disabled: spec === undefined,
          title: 'Switch to target (Enter)',
          onclick: (e: MouseEvent) => {
            e.stopPropagation();
            submit();
          },
        },
        'Go',
      ),
    ),
    m(
      '.pf-live-stream-hud__target-list',
      {
        'id': TARGET_LIST_ID,
        'role': 'listbox',
        'aria-label': 'Targets',
        // Shows the open target, or else one of its job, in the middle.
        'oncreate': (vnode: m.VnodeDOM) => {
          const list = vnode.dom;
          const row =
            list.querySelector('.pf-live-stream-hud__target-item--selected') ??
            list.querySelector('[data-template-match="true"]');
          if (!(list instanceof HTMLElement) || !(row instanceof HTMLElement)) {
            return;
          }
          list.scrollTop = Math.max(
            0,
            row.offsetTop - list.clientHeight / 2 + row.offsetHeight / 2,
          );
        },
      },
      hudState.targetSearchDirty &&
        query !== '' &&
        !hasExactTarget(targets, query) &&
        spec !== undefined &&
        m(
          '.pf-live-stream-hud__target-item.pf-live-stream-hud__target-item--custom',
          {role: 'option', onclick: submit},
          m(
            '.pf-live-stream-hud__target-item-main',
            m(
              'span.pf-live-stream-hud__target-item-short',
              `Switch to target: ${query}`,
            ),
            m(
              'span.pf-live-stream-hud__target-item-desc',
              '\u200EPress Enter or click to switch\u200E',
            ),
          ),
        ),
      shown.map((_, i) => renderTargetItem(props, shown, i, activeBase)),
    ),
  );
}

// The targets that the menu lists: all until the search is edited.
function listedTargets(props: LiveHudProps): readonly Target[] {
  const {hudState, targets} = props;
  if (!hudState.targetSearchDirty) return targets;
  return filterTargets(targets, hudState.targetSearchInput);
}

// Highlights the listed target `step` rows from the highlighted one, else
// from the open one, else from the top (↓) or bottom (↑), and scrolls to it.
function highlightTarget(props: LiveHudProps, step: 1 | -1) {
  const {hudState} = props;
  const shown = listedTargets(props);
  if (shown.length === 0) return;
  const indexOf = (id?: string) => shown.findIndex((t) => t.id === id);
  let from = indexOf(hudState.highlightedTarget);
  if (from < 0) from = indexOf(props.target?.id);
  if (from < 0) from = step > 0 ? -1 : shown.length;
  const index = clamp(from + step, 0, shown.length - 1);
  hudState.highlightedTarget = shown[index]!.id;

  // The row is already rendered; only its highlight waits for the redraw.
  scrollIntoList(document.getElementById(targetOptionId(index)));
}

// Scrolls the list of `row`, if any, as little as needed to show it.
function scrollIntoList(row: HTMLElement | null) {
  const list = row?.parentElement;
  if (row === null || !list) return;
  const bottom = row.offsetTop + row.offsetHeight;
  if (row.offsetTop < list.scrollTop) {
    list.scrollTop = row.offsetTop;
  } else if (bottom > list.scrollTop + list.clientHeight) {
    list.scrollTop = bottom - list.clientHeight;
  }
}

// Opens a listed target, like a click on its row.
function openListedTarget(props: LiveHudProps, id: string) {
  props.hudState.isTargetMenuOpen = false;
  props.onOpenTarget(id);
}

// The id of the target list, for the search box's aria-controls.
const TARGET_LIST_ID = 'pf-live-stream-hud__target-list';

// The id of row `index` of the target list, for aria-activedescendant.
function targetOptionId(index: number): string {
  return `pf-live-stream-hud__target-option-${index}`;
}

// Row `index` of the listed targets `shown`.
function renderTargetItem(
  props: LiveHudProps,
  shown: readonly Target[],
  index: number,
  activeBase: string,
): m.Children {
  const {hudState} = props;
  const t = shown[index]!;
  const isSelected = t.id === props.target?.id;
  const isTemplateMatch =
    !isSelected && activeBase !== '' && targetBaseName(t) === activeBase;
  const link = targetLink(t);
  return m(
    '.pf-live-stream-hud__target-item',
    {
      'key': t.id,
      'id': targetOptionId(index),
      'role': 'option',
      'aria-selected': isSelected ? 'true' : 'false',
      // The description shows on hover.
      'title': targetDescription(t),
      'className': classNames(
        isSelected && 'pf-live-stream-hud__target-item--selected',
        t.id === hudState.highlightedTarget &&
          'pf-live-stream-hud__target-item--highlighted',
      ),
      'data-template-match': isTemplateMatch ? 'true' : undefined,
      'onclick': () => openListedTarget(props, t.id),
    },
    m(
      '.pf-live-stream-hud__target-item-main',
      m('span.pf-live-stream-hud__target-item-short', targetShortName(t)),
      shown.length === 1 &&
        isIndexed(t) &&
        m(
          'span.pf-live-stream-hud__target-item-count',
          `Total count: ${t.task_count}`,
        ),
    ),
    link !== undefined &&
      m(
        'a.pf-live-stream-hud__target-item-link',
        {
          href: link,
          target: '_blank',
          rel: 'noopener noreferrer',
          title: 'Open target link',
          onclick: (e: MouseEvent) => e.stopPropagation(),
        },
        m(Icon, {icon: 'open_in_new'}),
      ),
  );
}

function renderMoreDropdown(props: LiveHudProps): m.Children {
  const {hudState, settings} = props;
  const choose = (action: () => void) => (e: MouseEvent) => {
    blurTarget(e);
    hudState.isMoreMenuOpen = false;
    action();
  };
  return m(
    '.pf-live-stream-hud__more-dropdown',
    m(
      'button.pf-live-stream-hud__btn.pf-live-stream-hud__more-trigger',
      {
        className: classNames(
          hudState.isMoreMenuOpen && 'pf-live-stream-hud__btn--active',
        ),
        title: 'More actions',
        onclick: (e: MouseEvent) => {
          blurTarget(e);
          hudState.isMoreMenuOpen = !hudState.isMoreMenuOpen;
          if (hudState.isMoreMenuOpen) hudState.isTargetMenuOpen = false;
        },
      },
      '⋯',
    ),
    hudState.isMoreMenuOpen &&
      m(
        '.pf-live-stream-hud__more-menu',
        moreActions(props).map((action) =>
          m(
            'button.pf-live-stream-hud__more-item',
            {title: action.title, onclick: choose(action.run)},
            m('span.pf-live-stream-hud__more-item-icon', action.icon),
            m('span.pf-live-stream-hud__more-item-label', action.label),
          ),
        ),
        m('.pf-live-stream-hud__more-divider'),
        renderSettings(settings),
      ),
  );
}

interface HudAction {
  readonly icon: string;
  readonly label: string;
  readonly title?: string;
  run(): void;
}

// The actions of the row's ⋯ menu, which the panel shows as buttons.
function moreActions(props: LiveHudProps): readonly HudAction[] {
  return [
    {icon: '↻', label: 'Reconnect', run: props.onReconnect},
    {
      icon: '⬇',
      label: 'Download Trace',
      title: 'Download a Perfetto trace (.pftrace) to load later',
      run: props.onDownload,
    },
  ];
}

// The settings: in the row's ⋯ menu, and the panel's last section.
function renderSettings(settings: LiveSettings): m.Children {
  return [
    renderSelect(
      'Poll Interval',
      settings.updateInterval,
      UPDATE_INTERVALS,
      (o) => `${Number(o).toFixed(o === '0.25' ? 2 : 1)}s`,
    ),
    renderSelect(
      'Buffer Window',
      settings.bufferDelay,
      BUFFER_DELAYS,
      (o) => `${o}s`,
    ),
    renderSelect('History', settings.retention, RETENTIONS, (o) =>
      Number(o) % 60 === 0 ? `${Number(o) / 60}h` : `${o}min`,
    ),
  ];
}

interface ChoiceSetting<T extends string> {
  get(): T;
  set(value: T): void;
}

// A setting with a few choices. Blurs after a choice, so that keys go back to
// the timeline, e.g. Space to follow.
function renderSelect<T extends string>(
  label: string,
  setting: ChoiceSetting<T>,
  options: readonly T[],
  format: (option: T) => string,
): m.Children {
  return m(
    'label.pf-live-stream-hud__more-interval-row',
    m('span.pf-live-stream-hud__more-interval-label', label),
    m(
      'select.pf-live-stream-hud__select.pf-live-stream-hud__select--interval',
      {
        value: setting.get(),
        onchange: (e: Event) => {
          const select = e.target;
          if (!(select instanceof HTMLSelectElement)) return;
          const option = options.find((o) => o === select.value);
          if (option !== undefined) setting.set(option);
          select.blur();
        },
      },
      options.map((o) => m('option', {value: o}, format(o))),
    ),
  );
}

// Blurs a clicked button, so that Space goes back to the timeline.
function blurTarget(e: Event) {
  if (e.currentTarget instanceof HTMLElement) e.currentTarget.blur();
}
