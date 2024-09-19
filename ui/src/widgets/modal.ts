// Copyright (C) 2019 The Android Open Source Project
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
import {defer} from '../base/deferred';
import {scheduleFullRedraw} from './raf';
import {Icon} from './icon';

// This module deals with modal dialogs. Unlike most components, here we want to
// render the DOM elements outside of the corresponding vdom tree. For instance
// we might want to instantiate a modal dialog all the way down from a nested
// Mithril sub-component, but we want the result dom element to be nested under
// the root <body>.

// Usage:
// Full-screen modal use cases (the most common case)
// --------------------------------------------------
// - app.ts calls maybeRenderFullscreenModalDialog() when rendering the
//   top-level vdom, if a modal dialog is created via showModal()
// - The user (any TS code anywhere) calls showModal()
// - showModal() takes either:
//   - A static set of mithril vnodes (for cases when the contents of the modal
//     dialog is static and never changes)
//   - A function, invoked on each render pass, that returns mithril vnodes upon
//     each invocation.
//   - See examples in widgets_page.ts for both.
//
// Nested modal use-cases
// ----------------------
// A modal dialog can be created in a "positioned" layer (e.g., any div that has
// position:relative|absolute), so it's modal but only within the scope of that
// layer.
// In this case, just ust the Modal class as a standard mithril component.
// showModal()/closeModal() are irrelevant in this case.

export interface ModalAttrs {
  title: string;
  buttons?: ModalButton[];
  vAlign?: 'MIDDLE' /* default */ | 'TOP';

  // Used to disambiguate between different modal dialogs that might overlap
  // due to different client showing modal dialogs at the same time. This needs
  // to match the key passed to closeModal() (if non-undefined). If the key is
  // not provided, showModal will make up a random key in the showModal() call.
  key?: string;

  // A callback that is called when the dialog is closed, whether by pressing
  // any buttons or hitting ESC or clicking outside of the modal.
  onClose?: () => void;

  // The content/body of the modal dialog. This can be either:
  // 1. A static set of children, for simple dialogs which content never change.
  // 2. A factory method that returns a m() vnode for dyamic content.
  content?: m.Children | (() => m.Children);
}

export interface ModalButton {
  text: string;
  primary?: boolean;
  id?: string;
  action?: () => void;
}

// Usually users don't need to care about this class, as this is instantiated
// by showModal. The only case when users should depend on this is when they
// want to nest a modal dialog in a <div> they control (i.e. when the modal
// is scoped to a mithril component, not fullscreen).
export class Modal implements m.ClassComponent<ModalAttrs> {
  onbeforeremove(vnode: m.VnodeDOM<ModalAttrs>) {
    const removePromise = defer<void>();
    vnode.dom.addEventListener('animationend', () => removePromise.resolve());
    vnode.dom.classList.add('modal-fadeout');

    // Retuning `removePromise` will cause Mithril to defer the actual component
    // removal until the fade-out animation is done. onremove() will be invoked
    // after this.
    return removePromise;
  }

  onremove(vnode: m.VnodeDOM<ModalAttrs>) {
    if (vnode.attrs.onClose !== undefined) {
      // The onClose here is the promise wrapper created by showModal(), which
      // in turn will: (1) call the user's original attrs.onClose; (2) resolve
      // the promise returned by showModal().
      vnode.attrs.onClose();
      scheduleFullRedraw();
    }
  }

  oncreate(vnode: m.VnodeDOM<ModalAttrs>) {
    if (vnode.dom instanceof HTMLElement) {
      // Focus the newly created dialog, so that we react to Escape keydown
      // even if the user has not clicked yet on any element.
      // If there is a primary button, focus that, so Enter does the default
      // action. If not just focus the whole dialog.
      const primaryBtn = vnode.dom.querySelector('.modal-btn-primary');
      if (primaryBtn) {
        (primaryBtn as HTMLElement).focus();
      } else {
        vnode.dom.focus();
      }
      // If the modal dialog is instantiated in a tall scrollable container,
      // make sure to scroll it into the view.
      vnode.dom.scrollIntoView({block: 'center'});
    }
  }

  view(vnode: m.Vnode<ModalAttrs>) {
    const attrs = vnode.attrs;

    const buttons: m.Children = [];
    for (const button of attrs.buttons || []) {
      buttons.push(
        m(
          'button.modal-btn',
          {
            class: button.primary ? 'modal-btn-primary' : '',
            id: button.id,
            onclick: () => {
              closeModal(attrs.key);
              if (button.action !== undefined) button.action();
            },
          },
          button.text,
        ),
      );
    }

    const aria = '[aria-labelledby=mm-title][aria-model][role=dialog]';
    const align = attrs.vAlign === 'TOP' ? '.modal-dialog-valign-top' : '';
    return m(
      '.modal-backdrop',
      {
        onclick: this.onBackdropClick.bind(this, attrs),
        onkeyup: this.onBackdropKeyupdown.bind(this, attrs),
        onkeydown: this.onBackdropKeyupdown.bind(this, attrs),
        tabIndex: 0,
      },
      m(
        `.modal-dialog${align}${aria}`,
        m(
          'header',
          m('h2', {id: 'mm-title'}, attrs.title),
          m(
            'button[aria-label=Close Modal]',
            {onclick: () => closeModal(attrs.key)},
            m(Icon, {icon: 'close'}),
          ),
        ),
        m('main', vnode.children),
        buttons.length > 0 ? m('footer', buttons) : null,
      ),
    );
  }

  onBackdropClick(attrs: ModalAttrs, e: MouseEvent) {
    e.stopPropagation();
    // Only react when clicking on the backdrop. Don't close if the user clicks
    // on the dialog itself.
    const t = e.target;
    if (t instanceof Element && t.classList.contains('modal-backdrop')) {
      closeModal(attrs.key);
    }
  }

  onBackdropKeyupdown(attrs: ModalAttrs, e: KeyboardEvent) {
    e.stopPropagation();
    if (e.key === 'Escape' && e.type !== 'keyup') {
      closeModal(attrs.key);
    }
  }
}

// Set by showModal().
let currentModal: ModalAttrs | undefined = undefined;
let generationCounter = 0;

// This should be called only by app.ts and nothing else.
// This generates the modal dialog at the root of the DOM, so it can overlay
// on top of everything else.
export function maybeRenderFullscreenModalDialog() {
  // We use the generation counter as key to distinguish between: (1) two render
  // passes for the same dialog vs (2) rendering a new dialog that has been
  // created invoking showModal() while another modal dialog was already being
  // shown.
  if (currentModal === undefined) return [];
  let children: m.Children;
  if (currentModal.content === undefined) {
    children = null;
  } else if (typeof currentModal.content === 'function') {
    children = currentModal.content();
  } else {
    children = currentModal.content;
  }
  return [m(Modal, currentModal, children)];
}

// Shows a full-screen modal dialog.
export async function showModal(userAttrs: ModalAttrs): Promise<void> {
  const returnedClosePromise = defer<void>();
  const userOnClose = userAttrs.onClose ?? (() => {});

  // If the user doesn't specify a key (to match the closeModal), generate a
  // random key to distinguish two showModal({key:undefined}) calls.
  const key = userAttrs.key ?? `${++generationCounter}`;
  const attrs: ModalAttrs = {
    ...userAttrs,
    key,
    onClose: () => {
      userOnClose();
      returnedClosePromise.resolve();
    },
  };
  currentModal = attrs;
  scheduleFullRedraw();
  return returnedClosePromise;
}

// Technically we don't need to redraw the whole app, but it's the more
// pragmatic option. This is exposed to keep the plugin code more clear, so it's
// evident why a redraw is requested.
export function redrawModal() {
  if (currentModal !== undefined) {
    scheduleFullRedraw();
  }
}

// Closes the full-screen modal dialog (if any).
// `key` is optional: if provided it will close the modal dialog only if the key
// matches. This is to avoid accidentally closing another dialog that popped
// in the meanwhile. If undefined, it closes whatever modal dialog is currently
// open (if any).
export function closeModal(key?: string) {
  if (
    currentModal === undefined ||
    (key !== undefined && currentModal.key !== key)
  ) {
    // Somebody else closed the modal dialog already, or opened a new one with
    // a different key.
    return;
  }
  currentModal = undefined;
  scheduleFullRedraw();
}

export function getCurrentModalKey(): string | undefined {
  return currentModal?.key;
}
