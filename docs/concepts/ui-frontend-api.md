# Frontend Main API

This document provides an overview of the `Frontend` API in Perfetto UI, as defined in [`ui/src/frontend/frontend.ts`](../../ui/src/frontend/frontend.ts).
This API is the main application entry-point for the Perfetto UI and may be used by host (embedding) applications to customize various aspects of Perfetto UI's behavior, especially for advanced integration scenarios.

## Overview

The `Frontend` class provides for the main start-up sequence of the Perfetto UI.
The various steps in this sequence are factored out into protected API methods that a host application can extend or override to control or redefined specific Perfetto UI behaviors.
The primary intended use case is in applications that take care of such concerns as acquiring and managing Perfetto traces but wish to reuse/embed the Perfetto UI for presentation of those traces to the user.

When a host application creates and starts the `Frontend`, it must be sure to do this *before* the main [Perfetto UI module](../../ui/src/frontend/index.ts) is loaded or, better, ensure that that main module is not loaded at all as it would not be needed.

Host applications can specialize the `Frontend` to customize the following aspects of the Perfetto UI app:

- **Custom Error Handling**: Suppress Perfetto UI's internal error handling, deferring to whatever the host application does in that regard.
- **UI Rendering Control**: Prevent the start-up of the Perfetto UI app from rendering its main interface, allowing the host application can selectively instantiate components.
- **Content Security Policy Strategy**: Customization of how the CSP is installed in the application, with optional filtering of the rules.
- **Storage Customization**: Prefix cache storage keys for coexistence with other applications' caches and for certain application frameworks' restrictions.
- **Routing Hooks**: Intercept or override URL navigation and route handling.
- **Custom PostMessage Handling**: Process unrecognized messages posted to the window.
- **Preloaded Trace Handling**: Indicate how Perfetto should deal with traces pre-loaded in the trace processor backend.
- **App State Restoration**: Delegate Perfetto app state persistence to the host application.

### Example

Suppose you have an application integrating Perfetto:

```typescript
// perfetto-setup.ts
import type {AppImpl} from 'perfetto/ui/dist/core/app_impl';
import {Frontend} from 'perfetto/ui/dist/frontend/frontend';

export class MyFrontend extends Frontend {
  override protected installErrorHandlers(): void {
    // I do my own error handling
  }

  override protected mountMainUI(): Disposable {
    // I instantiate the UI at my own place in the DOM
    return {
      [Symbol.dispose]: () => {},
    };
  }

  override protected createApp(args: AppInitArgs): AppImpl {
    return super.createApp({
      ...args,
      // Don't get initial route args from the window location
      initialRouteArgs: {},
    });
  }

  // ... other overrides

  override start(): Promise<void> {
    // Set a custom cache prefix for my application
    setCachePrefix('https:/');
    return super.start();
  }
}
```

Then, in your application entrypoint:

```typescript
// index.ts
import {MyFrontend} from './perfetto-setup';

// ...

await new MyFrontend().start(); // Initialize the Perfetto UI
```

This ensures correct initialization of Perfetto's frontend with your application's customizations.
