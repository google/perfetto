import m from 'mithril';
import {Icon} from '../widgets/icon';
import {Button} from '../widgets/button';
import {getOrCreate} from '../base/utils';


export const SIDEBAR_SECTIONS = {
    home: {
      title: 'Home',
      summary: '',
      defaultCollapsed: false,
    },
    bigtrace: {
      title: 'BigTrace',
      summary: 'Query and analyze large traces',
      defaultCollapsed: false,
    },
    settings: {
      title: 'Settings',
      summary: 'Customize your BigTrace experience',
      defaultCollapsed: false,
    },
  } as const;

export type SidebarSections = keyof typeof SIDEBAR_SECTIONS;

export type SidebarMenuItem = {
    readonly section: SidebarSections;
    readonly text: string;
    readonly href: string;
    readonly icon: string;
    readonly active: boolean;
    readonly onclick: () => void;
};

export interface SidebarAttrs {
    items: SidebarMenuItem[];
    onToggleSidebar: () => void;
}

export class Sidebar implements m.ClassComponent<SidebarAttrs> {
    private _sectionExpanded = new Map<string, boolean>();

    view({attrs}: m.CVnode<SidebarAttrs>) {
        return m(
            'nav.pf-sidebar',
            {
              style: {
                width: '200px',
                flexShrink: 0,
              },
            },
            [
              m(
                'header',
                {
                  style: {
                    padding: '16px',
                    borderBottom: '1px solid var(--pf-color-border)',
                    display: 'flex',
                    alignItems: 'center',
                    justifyContent: 'space-between',
                  },
                },
                [
                  m('h1', {style: {margin: 0, fontSize: '18px'}}, 'BigTrace'),
                  m(Button, {
                    icon: 'menu',
                    onclick: attrs.onToggleSidebar,
                  }),
                ],
              ),
              m(
                '.pf-sidebar__scroll',
                m('.pf-sidebar__scroll-container',
                    Object.keys(SIDEBAR_SECTIONS).map((sectionId) =>
                        this.renderSection(sectionId as SidebarSections, attrs.items)
                    )
                ),
              ),
            ],
          );
    }

    private renderSection(sectionId: SidebarSections, items: SidebarMenuItem[]) {
        const section = SIDEBAR_SECTIONS[sectionId];
        const menuItems = items.filter(item => item.section === sectionId).map(item => this.renderItem(item));

        if (menuItems.length === 0) return undefined;

        const expanded = getOrCreate(
            this._sectionExpanded,
            sectionId,
            () => !section.defaultCollapsed,
          );

        return m(
            `section${expanded ? '.pf-sidebar__section--expanded' : ''}`,
            m(
              '.pf-sidebar__section-header',
              {
                onclick: () => {
                  this._sectionExpanded.set(sectionId, !expanded);
                },
              },
              m('h1', {title: section.title}, section.title),
            ),
            m('.pf-sidebar__section-content', m('ul', menuItems)),
          );
    }

    private renderItem(item: SidebarMenuItem) {
        return m(
            'li',
            m(
              'a',
              {
                class: item.active ? 'active' : '',
                onclick: item.onclick,
                href: item.href,
              },
              [m(Icon, {icon: item.icon}), item.text],
            ),
          );
    }
}
