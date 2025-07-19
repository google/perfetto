# EmbedderContext API

This document provides an overview of the `EmbedderContext` API in Perfetto UI, as defined in [`ui/src/core/embedder.ts`](../../ui/src/core/embedder.ts).
This API allows host (embedding) applications to customize various aspects of Perfetto UI's behavior, especially for advanced integration scenarios.

## Overview

The `EmbedderContext` interface provides a collection of optional hooks, flags, and handlers that a host application can implement to control or override specific Perfetto UI behaviors.
The primary intended use case is in applications that take care of such concerns as acquiring and managing Perfetto traces but wish to reuse/embed the Perfetto UI for presentation of those traces to the user.

The context object must be set *before* loading the main Perfetto UI module to ensure proper integration.

Host applications can configure the `EmbedderContext` to customize the following aspects of the Perfetto UI app:

- **Custom Error Handling**: Suppress Perfetto UI's internal error handling, deferring to whatever the host application does in that regard.
- **UI Rendering Control**: Prevent the start-up of the Perfetto UI app from rendering its main interface, allowing the host application can selectively instantiate components.
- **Content Security Policy Strategy**: Customization of how the CSP is installed in the application, with optional filtering of the rules.
- **Storage Customization**: Prefix cache storage keys for coexistence with other applications' caches and for certain application frameworks' restrictions.
- **Routing Hooks**: Intercept or override URL navigation and route handling.
- **Custom PostMessage Handling**: Process unrecognized messages posted to the window.
- **Preloaded Trace Handling**: Indicate how Perfetto should deal with traces pre-loaded in the trace processor backend.
- **App State Restoration**: Delegate Perfetto app state persistence to the host application.

## Setting the Embedder Context

The host application _must_ set its embedder context via the `setEmbedderContext()` function _before_ the Perfetto UI app is initialized.
This function may be called only once.
Attempting to replace an established embedder context throws an error.

Since Perfetto UI's main entry point (e.g., `frontend/index.ts`) references the embedder context as it initializes, the safest approach is:

- define your embedder context and call `setEmbedderContext()` in a dedicated module.
Ideally it will not need to import any other Perfetto UI modules than `embedder.ts` and a few minimal dependencies for types and APIs such as `Router` and `RafScheduler`
- ensure that this module is imported by your application before any Perfetto source modules

### Example

Suppose you have an application integrating Perfetto:

```typescript
// perfetto-setup.ts
import {setEmbedderContext} from 'perfetto/ui/dist/core/embedder';

setEmbedderContext({
  suppressErrorHandling: true,
  suppressMainUi: true,
  cachePrefix: 'https:/',
  // ...other overrides
});
```

Then, in your application entrypoint:

```typescript
// index.ts
import './perfetto-setup';        // <-- Import this first!
import 'perfetto/ui/dist/frontend/index'; // Now load Perfetto UI
```

This ensures the embedder context is established before Perfetto's frontend initialization.
