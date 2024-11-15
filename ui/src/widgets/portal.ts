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

import m from 'mithril';

type Style = string | Partial<CSSStyleDeclaration>;

export interface MountOptions {
  // Optionally specify an element in which to place our portal.
  // Defaults to body.
  container?: Element;
}

export interface PortalAttrs {
  // Space delimited class list forwarded to our portal element.
  className?: string;
  // Inline styles forwarded to our portal element.
  style?: Style;
  // Called before our portal is created, allowing customization of where in the
  // DOM the portal is mounted.
  // The dom parameter is a dummy element representing where the portal would be
  // located if it were rendered into the normal tree hierarchy.
  onBeforeContentMount?: (dom: Element) => MountOptions;
  // Called after our portal is created and its content rendered.
  onContentMount?: (portalElement: HTMLElement) => void;
  // Called after our portal's content is updated.
  onContentUpdate?: (portalElement: HTMLElement) => void;
  // Called before our portal is removed.
  onContentUnmount?: (portalElement: HTMLElement) => void;
}

// A portal renders children into a a div outside of the normal hierarchy of the
// parent component, usually in order to stack elements on top of others.
// Useful for creating overlays, dialogs, and popups.
export class Portal implements m.ClassComponent<PortalAttrs> {
  private portalElement?: HTMLElement;
  private containerElement?: Element;
  private contentComponent: m.Component;

  constructor({children}: m.CVnode<PortalAttrs>) {
    // Create a temporary component that we can mount in oncreate, and unmount
    // in onremove, but inject the new portal content (children) into it each
    // render cycle. This is initialized here rather than in oncreate to avoid
    // having to make it optional or use assertExists().
    this.contentComponent = {view: () => children};
  }

  view() {
    // Dummy element renders nothing but permits DOM access in lifecycle hooks.
    return m('span', {style: {display: 'none'}});
  }

  oncreate({attrs, dom}: m.CVnodeDOM<PortalAttrs>) {
    const {
      onContentMount = () => {},
      onBeforeContentMount = (): MountOptions => ({}),
    } = attrs;

    const {container = document.body} = onBeforeContentMount(dom);
    this.containerElement = container;

    this.portalElement = document.createElement('div');
    container.appendChild(this.portalElement);
    this.applyPortalProps(attrs);

    m.mount(this.portalElement, this.contentComponent);

    onContentMount(this.portalElement);
  }

  onbeforeupdate({children}: m.CVnode<PortalAttrs>) {
    // Update the mounted content's view function to return the latest portal
    // content passed in via children, without changing the component itself.
    this.contentComponent.view = () => children;
  }

  onupdate({attrs}: m.CVnodeDOM<PortalAttrs>) {
    const {onContentUpdate = () => {}} = attrs;
    if (this.portalElement) {
      this.applyPortalProps(attrs);
      onContentUpdate(this.portalElement);
    }
  }

  private applyPortalProps(attrs: PortalAttrs) {
    if (this.portalElement) {
      this.portalElement.className = attrs.className ?? '';
      Object.assign(this.portalElement.style, attrs.style);
    }
  }

  onremove({attrs}: m.CVnodeDOM<PortalAttrs>) {
    const {onContentUnmount = () => {}} = attrs;
    const container = this.containerElement ?? document.body;
    if (this.portalElement) {
      if (container.contains(this.portalElement)) {
        onContentUnmount(this.portalElement);
        // Rendering null ensures previous vnodes are removed properly.
        m.mount(this.portalElement, null);
        container.removeChild(this.portalElement);
      }
    }
  }
}
