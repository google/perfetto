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

import m from "mithril";
import "./nav.scss";
import { AppImpl } from "../../../ui/src/core/app_impl";
import { Icon } from "../../../ui/src/widgets/icon";
import { classNames } from "../../../ui/src/base/classnames";
import { memoryExplorerHref } from "../../../ui/src/plugins/dev.perfetto.Memscope/memory_explorer/route";

interface NavTab {
  readonly label: string;
  readonly href: string;
  readonly page: string;
}

// Memscope has no sidebar: the brand goes home and the tabs switch between
// the product's top level pages.
export function renderMemscopeNav(): m.Children {
  const app = AppImpl.instance;
  const page = app.getCurrentRoute().page;
  const tabs: NavTab[] = [
    { label: "Live", href: "#!/memscope", page: "/memscope" },
  ];
  if (app.trace !== undefined) {
    tabs.push({
      label: "Memory Explorer",
      href: memoryExplorerHref(),
      page: "/memory",
    });
  }
  return m(
    ".pf-memscope-nav",
    m(
      "a.pf-memscope-nav__brand",
      { href: "#!/" },
      m(Icon, { icon: "memory" }),
      "Memscope",
    ),
    tabs.map((t) =>
      m(
        "a.pf-memscope-nav__tab",
        {
          href: t.href,
          className: classNames(
            page === t.page && "pf-memscope-nav__tab--active",
          ),
        },
        t.label,
      ),
    ),
  );
}
