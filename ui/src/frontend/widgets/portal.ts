// Copyright (C) 2023 The Android Open Source Project
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

import * as m from 'mithril';

type Style = string|Partial<CSSStyleDeclaration>;

export interface PortalAttrs {
  // Inline styles forwarded to our portal container
  style?: Style;
  // Called after content is mounted in the portal
  onContentMount?: (rootElement: HTMLElement) => void;
  // Called after the content is unmounted from the portal
  onContentUnmount?: (rootElement: HTMLElement) => void;
  // Called when the content is updated
  onContentUpdate?: () => void;
}

// A portal renders children into a a div outside of the normal hierarchy of the
// parent component.
// For now, this implementation adds a new div to the root of the document, and
// mounts its children into this div.
// The main reason for doing this is to draw a floating item over the top of the
// other elements, such as a popover or a menu.
// If there are no manually set z-indexes the portal would appear over the top
// of the rest of the elements on the page by virtue of the fact that it's the
// last element in the document.
// If manual z-indexes have been set on other page elements, then this one's
// z-index might need to be set to be above the others, but at least the
// stacking contexts are easier to manage.
export class Portal implements m.ClassComponent<PortalAttrs> {
  private rootElement?: HTMLElement;
  private component?: m.Component;

  oncreate({attrs, children}: m.VnodeDOM<PortalAttrs, this>) {
    const {
      onContentUpdate = () => {},
      onContentMount = (_) => {},
    } = attrs;
    // Create a new div, assigning styles from our attrs
    const rootElement = document.createElement('div');
    Object.assign(rootElement.style, attrs.style);

    // Attach it to the body of the document
    document.body.appendChild(rootElement);

    this.rootElement = rootElement;

    // Create a proxy component which just returns the children
    this.component = {
      view: () => children,
      onupdate: () => onContentUpdate(),
    };

    // Mount this component in the root element
    m.mount(this.rootElement, this.component);

    // At this point, all children of the portal will have had their
    // oncreate() function calls which the owner of portal has no visibility
    // into so we can notify it through the callback if one exists
    onContentMount(this.rootElement);
  }

  onbeforeupdate({children}: m.Vnode<PortalAttrs>) {
    // We need to jump in before view is called to replace our pseudo view
    // component, updating its children to ours...
    if (!this.component) {
      return false;
    }
    this.component.view = () => children;
    return true;
  }

  onupdate({attrs}: m.VnodeDOM<PortalAttrs, this>) {
    // We can now update the styles
    Object.assign(this.rootElement!.style, attrs.style);
  }

  onremove({attrs}: m.VnodeDOM<PortalAttrs, this>) {
    const {onContentUnmount = () => {}} = attrs;
    // If the body contains our rootelement, unmount it (mount null?) and
    // remove the div from the root element
    if (this.rootElement) {
      if (document.body.contains(this.rootElement)) {
        onContentUnmount(this.rootElement);
        m.mount(this.rootElement!, null);
        document.body.removeChild(this.rootElement);
      }
    }
  }

  view(_: m.Vnode<PortalAttrs, this>): void|m.Children {
    // We can return nothing here, we just want to get involved with
    // lifecycle events
    return null;
  }
}
