import m from 'mithril';

interface Attrs {
  onDisable: () => void;
}

export const TimelineSyncEnabledDialog = {
  view: ({attrs}: m.Component<Attrs>) => {
    return m(
      'div',
      {
        style: {
          position: 'fixed',
          top: '20px',
          right: '20px',
          backgroundColor: 'white',
          border: '1px solid gray',
          padding: '20px',
          zIndex: 1000,
        },
      },
      [
        m('p', 'Timeline sync is currently enabled.'),
        m(
          'button',
          {onclick: attrs.onDisable},
          'Disable Timeline Sync',
        ),
      ],
    );
  },
};
