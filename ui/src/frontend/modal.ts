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


// This module deals with modal dialogs. Unlike most components, here we want to
// render the DOM elements outside of the corresponding vdom tree. For instance
// we might want to instantiate a modal dialog all the way down from a nested
// Mithril sub-component, but we want the result dom element to be nested under
// the root <body>.
//
// This is achieved by splitting:
// 1. ModalContainer: it's the placeholder (e.g., the thing that should be added
//    under <body>) where the DOM elements will be rendered into. This is NOT
//    a mithril component itself.
// 2. Modal: is the Mithril component with the actual VDOM->DOM handling.
//    This can be used directly in the cases where the modal DOM should be
//    placed presicely where the corresponding Mithril VDOM is.
//    In turn this is split into Modal and ModalImpl, to deal with fade-out, see
//    comments around onbeforeremove.

// Usage (in the case of DOM not matching VDOM):
// - Create a ModalContainer instance somewhere (e.g. a singleton for the case
//   of the full-screen modal dialog).
// - In the view() method of the component that should host the DOM elements
//   (e.g. in the root pages.ts) do the following:
//   view() {
//     return m('main',
//        m('h2', ...)
//        m(modalContainerInstance.mithrilComponent);
//   }
//
// - In the view() method of the nested component that wants to show the modal
//   dialog do the following:
//   view() {
//     if (shouldShowModalDialog) {
//       modalContainerInstance.update({title: 'Foo', content, buttons: ...});
//     }
//     return m('.nested-widget',
//       m('div', ...));
//   }
//
// For one-show use-cases it's still possible to just use:
// showModal({title: 'Foo', content, buttons: ...});

import m from 'mithril';

import {defer} from '../base/deferred';
import {assertExists, assertTrue} from '../base/logging';
import {raf} from '../core/raf_scheduler';

export interface ModalDefinition {
  title: string;
  content: m.Children|(() => m.Children);
  vAlign?: 'MIDDLE' /* default */ | 'TOP';
  buttons?: Button[];
  close?: boolean;
  onClose?: () => void;
}

export interface Button {
  text: string;
  primary?: boolean;
  id?: string;
  action?: () => void;
}

// The component that handles the actual modal dialog. Note that this uses
// position: absolute, so the modal dialog will be relative to the surrounding
// DOM.
// We need to split this into two components (Modal and ModalImpl) so that we
// can handle the fade-out animation via onbeforeremove. The problem here is
// that onbeforeremove is emitted only when the *parent* component removes the
// children from the vdom hierarchy. So we need a parent/child in our control to
// trigger this.
export class Modal implements m.ClassComponent<ModalDefinition> {
  private requestClose = false;

  close() {
    // The next view pass will kick-off the modalFadeOut CSS animation by
    // appending the .modal-hidden CSS class.
    this.requestClose = true;
    raf.scheduleFullRedraw();
  }

  view(vnode: m.Vnode<ModalDefinition>) {
    if (this.requestClose || vnode.attrs.close) {
      return null;
    }

    return m(ModalImpl, {...vnode.attrs, parent: this} as ModalImplAttrs);
  }
}

interface ModalImplAttrs extends ModalDefinition {
  parent: Modal;
}

// The component that handles the actual modal dialog. Note that this uses
// position: absolute, so the modal dialog will be relative to the surrounding
// DOM.
class ModalImpl implements m.ClassComponent<ModalImplAttrs> {
  private parent ?: Modal;
  private onClose?: () => void;

  view({attrs}: m.Vnode<ModalImplAttrs>) {
    this.onClose = attrs.onClose;
    this.parent = attrs.parent;

    const buttons: Array<m.Vnode<Button>> = [];
    for (const button of attrs.buttons || []) {
      buttons.push(m('button.modal-btn', {
        class: button.primary ? 'modal-btn-primary' : '',
        id: button.id,
        onclick: () => {
          attrs.parent.close();
          if (button.action !== undefined) button.action();
        },
      },
      button.text));
    }

    const aria = '[aria-labelledby=mm-title][aria-model][role=dialog]';
    const align = attrs.vAlign === 'TOP' ? '.modal-dialog-valign-top' : '';
    return m(
        '.modal-backdrop',
        {
          onclick: this.onclick.bind(this),
          onkeyup: this.onkeyupdown.bind(this),
          onkeydown: this.onkeyupdown.bind(this),
          // onanimationend: this.onanimationend.bind(this),
          tabIndex: 0,
        },
        m(
            `.modal-dialog${align}${aria}`,
            m(
                'header',
                m('h2', {id: 'mm-title'}, attrs.title),
                m(
                    'button[aria-label=Close Modal]',
                    {onclick: () => attrs.parent.close()},
                    m.trust('&#x2715'),
                    ),
                ),
            m('main', this.renderContent(attrs.content)),
            m('footer', buttons),
            ));
  }

  private renderContent(content: m.Children|(() => m.Children)): m.Children {
    if (typeof content === 'function') {
      return content();
    } else {
      return content;
    }
  }

  oncreate(vnode: m.VnodeDOM<ModalImplAttrs>) {
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
      vnode.dom.scrollIntoView({'block': 'center'});
    }
  }


  onbeforeremove(vnode: m.VnodeDOM<ModalImplAttrs>) {
    const removePromise = defer<void>();
    vnode.dom.addEventListener('animationend', () => removePromise.resolve());
    vnode.dom.classList.add('modal-fadeout');

    // Retuning `removePromise` will cause Mithril to defer the actual component
    // removal until the fade-out animation is done.
    return removePromise;
  }

  onremove() {
    if (this.onClose !== undefined) {
      this.onClose();
      raf.scheduleFullRedraw();
    }
  }

  onclick(e: MouseEvent) {
    e.stopPropagation();
    // Only react when clicking on the backdrop. Don't close if the user clicks
    // on the dialog itself.
    const t = e.target;
    if (t instanceof Element && t.classList.contains('modal-backdrop')) {
      assertExists(this.parent).close();
    }
  }

  onkeyupdown(e: KeyboardEvent) {
    e.stopPropagation();
    if (e.key === 'Escape' && e.type !== 'keyup') {
      assertExists(this.parent).close();
    }
  }
}


// This is deliberately NOT a Mithril component. We want to manage the lifetime
// independently (outside of Mithril), so we can render from outside the current
// vdom sub-tree. ModalContainer instances should be singletons / globals.
export class ModalContainer {
  private attrs?: ModalDefinition;
  private generation = 1; // Start with a generation > `closeGeneration`.
  private closeGeneration = 0;

  // This is the mithril component that is exposed to the embedder (e.g. see
  // pages.ts). The caller is supposed to hyperscript this while building the
  // vdom tree that should host the modal dialog.
  readonly mithrilComponent = {
    container: this,
    view:
        function() {
          const thiz = this.container;
          const attrs = thiz.attrs;
          if (attrs === undefined) {
            return null;
          }
          return [m(Modal, {
            ...attrs,
            onClose: () => {
              // Remember the fact that the dialog was dismissed, in case the
              // whole ModalContainer gets instantiated from a different page
              // (which would cause the Modal to be destroyed and recreated).
              thiz.closeGeneration = thiz.generation;
              if (thiz.attrs?.onClose !== undefined) {
                thiz.attrs.onClose();
                raf.scheduleFullRedraw();
              }
            },
            close: thiz.closeGeneration === thiz.generation ? true :
                                                              attrs.close,
            key: thiz.generation,
          })];
        },
  };

  // This should be called to show a new modal dialog. The modal dialog will
  // be shown the next time something calls render() in a Mithril draw pass.
  // This enforces the creation of a new dialog.
  createNew(attrs: ModalDefinition) {
    this.generation++;
    this.updateVdom(attrs);
  }

  // Updates the current dialog or creates a new one if not existing. If a
  // dialog exists already, this will update the DOM of the existing dialog.
  // This should be called in at view() time by a nested Mithril component which
  // wants to display a modal dialog (but wants it to render outside).
  updateVdom(attrs: ModalDefinition) {
    this.attrs = attrs;
  }

  close() {
    this.closeGeneration = this.generation;
    raf.scheduleFullRedraw();
  }
}

// This is the default instance used for full-screen modal dialogs.
// page.ts calls `m(fullscreenModalContainer.mithrilComponent)` in its view().
export const fullscreenModalContainer = new ModalContainer();


export async function showModal(attrs: ModalDefinition): Promise<void> {
  // When using showModal, the caller cannot pass an onClose promise. It should
  // use the returned promised instead. onClose is only for clients using the
  // Mithril component directly.
  assertTrue(attrs.onClose === undefined);
  const promise = defer<void>();
  fullscreenModalContainer.createNew({
    ...attrs,
    onClose: () => promise.resolve(),
  });
  raf.scheduleFullRedraw();
  return promise;
}
