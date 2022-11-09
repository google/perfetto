import {PluginContext} from './plugin_api';
import {PluginManager, PluginRegistry} from './plugins';

test('can activate plugin', () => {
  const registry = new PluginRegistry();
  registry.register({
    pluginId: 'foo',
    activate: (_: PluginContext) => {},
  });
  const manager = new PluginManager(registry);
  manager.activatePlugin('foo');
  expect(manager.isActive('foo')).toBe(true);
});

test('can deactivate plugin', () => {
  const registry = new PluginRegistry();
  registry.register({
    pluginId: 'foo',
    activate: (_: PluginContext) => {},
  });
  const manager = new PluginManager(registry);
  manager.activatePlugin('foo');
  manager.deactivatePlugin('foo');
  expect(manager.isActive('foo')).toBe(false);
});
