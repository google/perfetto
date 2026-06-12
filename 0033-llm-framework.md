# Perfetto UI: LLM Framework (plumbing & config)

**Authors:** @stevegolton

**Status:** Draft

## Introduction

This doc proposes adding a provider-agnostic **LLM framework** to the Perfetto
UI core: the plumbing that lets any plugin or core feature request a model and
talk to a backend, plus the settings to configure those backends. It covers the
core LLM gateway (which lives in the UI core, not a plugin), the provider
plugins that implement individual backend APIs, and the configuration stack.

This is one of the docs split out from
[RFC-0029](0029-intelletto.md), the top-level coordination doc for the AI
assistant work, which remains the place for the overall motivation and the
component breakdown. This doc covers the **framework** itself; its consumers
live elsewhere - the assistant UX, agent loop and system prompt in
[RFC-0032](0032-embedded-assistant.md),
context injection in [RFC-0034](0034-context-injection.md), tools and skills in
[RFC-0035](0035-tools-and-skills.md), and the external-harness conduit in
[RFC-0036](0036-external-agent-mcp.md).

The wider motivation - who AI tooling in Perfetto serves and why - lives in
[RFC-0025](https://github.com/google/perfetto/discussions/5763). This doc takes
that case as given and covers the implementation of the framework layer.

### Scope

This doc covers the implementation in the OSS codebase - the provider-agnostic
plumbing. It makes no judgement about which backends are used or what data is
acceptable to send to them: trace contents are sent to whichever endpoint the
user configures, and any data-egress / privacy policy is a deployment concern
layered on top, out of scope here. Likewise, API key handling is up to the
user or specific deployment.

The framework is opt-in: it stays out of the way (and sends nothing anywhere)
until the user has explicitly set up or accepted a config, and there is a
prominent setting that turns all AI features off.

## Architecture: core gateway, plugin extensions

The LLM gateway lives in the **UI core**, not in a plugin. The core owns the
registries - providers, configs/models, tools, context providers, and
skills - and exposes registration through the standard plugin API, the same
way commands and settings registries work today. The pieces:

- **Core LLM gateway**: The common gateway that all plugins (and core
  features) requiring LLM services use to access models. Defines the settings
  where configs and models are set up, and hosts the registries that the
  assistant (and external harnesses) consume. Putting this in core means
  other features can make use of LLMs outside of the assistant - e.g. auto
  generating summaries on details panels or in the overview page - and
  registering a tool or context provider doesn't create a plugin-to-plugin
  dependency: every plugin already talks to core.
- `dev.perfetto.Intelletto`: The assistant plugin. Renders the assistant UI
  and runs the agent loop, consuming the core registries (tools, skills,
  context providers). Other plugins extend the assistant's capability by
  registering with core, not by depending on the assistant plugin. (Covered in
  the assistant sibling doc; named here only to show where it sits.)
- `dev.perfetto.LlmProviderXXX`: Backend implementations for the various
  endpoint APIs (e.g. Gemini, OpenAI, Anthropic, Prompt API, etc). Each
  registers a Provider with the core gateway.

How the pieces fit together:

```txt
          LLM backends (Gemini / OpenAI / Anthropic / ...)
                  ▲
                  │ HTTP
   ┌──────────────┴──────────────┐
   │ dev.perfetto.LlmProviderXXX │  (one per backend API)
   └──────────────▲──────────────┘
                  │ registers a Provider
   ┌──────────────┴──────────────┐   ┌───────────────────────┐
   │ UI core: LLM gateway        │   │ other plugins         │
   │ (configs/models + tool,     │◄──│ (request models, e.g. │
   │  context & skill registries)│   │ auto summaries;       │
   └──────────────▲──────────────┘   │ register tools, skills│
                  │                  │ & context providers)  │
                  │                  └───────────────────────┘
                  │ "the conversational model, please"
   ┌──────────────┴──────────────┐
   │ dev.perfetto.Intelletto     │
   │ (the assistant)             │
   └─────────────────────────────┘
```

## The Provider → Config → Model stack

The configuration stack is broken into three layers.

A **Provider** is code-behind implementing the hooks that describe how to talk
to a _kind_ of API (e.g. Gemini, OpenAI, Anthropic, Prompt API). Providers are
supplied by plugins, and one provider can back many configs. A provider must
be able to:

- **Create a conversation**: given the conversation invariants (system prompt,
  tool defs, model params), return a stateful `Conversation` handle. Each call
  to `conversation.sendTurn(newMessages, { signal })` appends only the new
  messages (the user's prompt, or tool results from the previous turn) and
  streams the model's response, exposing at least incremental text, tool calls,
  and a finish/stop reason. An optional per-turn `AbortSignal` cancels that
  in-flight turn (forwarded straight to `fetch`, or to a native session's own
  signal). The handle owns whatever per-conversation state the backend needs
  (see below), and `dispose()` tears the whole conversation down.
- **Translate tool definitions**: convert the gateway's neutral tool schema
  into the backend's native format (and down-convert the JSON Schema where the
  backend only accepts a subset, e.g. Gemini).
- **Translate tool use**: map native tool-call messages back to neutral
  `{name, args}`, and neutral tool results forward to the backend's native
  result format, preserving the call/result id threading.
- **Count/report tokens & errors**: surface usage and normalise backend errors
  (rate limit, auth, context-length) into a common shape.
- **Report a list of models**: makes the settings configuration a lot easier
  if you can see the list of models and their codenames.

A heavily simplified sketch of the Gemini provider, to show the shape (real
implementation: SSE parsing, error normalisation, abort handling, and
Gemini's tool-call quirks omitted):

```ts
export class GeminiProvider implements Provider {
  readonly id = "gemini";
  readonly label = "Google Gemini";
  readonly capabilities = { nativeToolCalling: true, streaming: true };

  // Drives the credentials form in settings.
  readonly credentialFields = [
    { key: "apiKey", label: "API key", secret: true, required: true },
  ];

  async listModels(creds: Credentials): Promise<AvailableModel[]> {
    const resp = await fetch(`${ENDPOINT}/models?key=${creds.apiKey}`);
    const json = await resp.json();
    return json.models.map((m) => ({ name: m.name.replace(/^models\//, "") }));
  }

  // Invariants (system prompt, tool defs, model params) are fixed for the
  // conversation's lifetime. Gemini is stateless, so the handle just remembers
  // the running message list and resends it each turn; a stateful backend would
  // hold a native session here instead.
  createConversation(opts: ConversationOpts, creds: Credentials): Conversation {
    return new GeminiConversation(opts, creds);
  }
}

class GeminiConversation implements Conversation {
  private history: Message[] = [];
  constructor(
    private opts: ConversationOpts,
    private creds: Credentials,
  ) {}

  // Append this turn's new messages (the user's prompt, or tool results) and
  // stream the reply. The multi-step tool-use loop lives in the consumer (the
  // agent), not here.
  async *sendTurn(
    messages: Message[],
    opts?: { signal?: AbortSignal },
  ): AsyncGenerator<StreamEvent> {
    this.history.push(...messages);

    const url =
      `${ENDPOINT}/models/${this.opts.params.modelName}` +
      `:streamGenerateContent?key=${this.creds.apiKey}`;

    const resp = await fetch(url, {
      method: "POST",
      signal: opts?.signal, // caller aborts this turn; dispose() kills the rest
      body: JSON.stringify({
        // Neutral -> native: full running history, system prompt, tool defs.
        contents: messagesToContents(this.history),
        systemInstruction: { parts: [{ text: this.opts.systemPrompt }] },
        tools: toolsToDeclarations(this.opts.tools),
      }),
    });

    // Native -> neutral: each streamed chunk becomes neutral events, and the
    // assembled reply is appended to history for the next turn.
    const reply: Message = { role: "assistant", parts: [] };
    for await (const part of streamParts(resp)) {
      if ("text" in part) {
        reply.parts.push({ text: part.text });
        yield { type: "text", text: part.text };
      } else if ("functionCall" in part) {
        const { name, args } = part.functionCall;
        reply.parts.push({ functionCall: part.functionCall });
        yield { type: "tool-call", call: { name, args } };
      }
    }
    this.history.push(reply);
    yield { type: "stop", reason: "end" };
  }

  dispose(): void {
    /* stateless backend: nothing to release */
  }
}
```

**Why a conversation handle, rather than a stateless `createStream(fullHistory)`
every turn?** Most backends today are stateless - the server keeps nothing, so
the handle simply holds the running transcript and resends it on each
`sendTurn`, which is identical wire behaviour to passing the whole history in,
just encapsulated. The payoff is the backends that _aren't_ stateless: notably
Chrome's built-in **Prompt API**, whose `LanguageModel` session retains context
across `prompt()` calls. Such a provider holds the native session inside the
handle and sends only the new turn, letting the backend own history and its own
prefix caching. Threading the entire conversation through a stateless call every
time would make that impossible to express. The consumer (the agent loop) still
owns the canonical transcript for rendering, export and reset; the `Conversation`
handle is just the wire-level conversation with one backend, and `dispose()`
releases it (closing a native session, aborting any in-flight request).

A **Config** is a data-only configured source: a reference to a provider,
the connection details (API key, base URL etc.), and the catalog of **Models**
it exposes. Each model entry carries its backend model id, model params
(temperature, thinking mode - where applicable), an optional model-specific
system prompt, and its role(s) - e.g. 'conversational' or 'flash' - which
define where the model shows up: a plugin asking the core gateway for a
model requests specific roles (the assistant only wants conversational
models). A config may also define a preferred model per role, which lets the
assistant 'just work' for Googlers when the config is pushed down by the
internal extension server. Configs can be supplied by extension servers, by
`embedders.ts`, or by end users via a setting (stored in localstorage).

Since a config is pure data, it's just a JSON blob:

```json
{
  "id": "google-ai-studio",
  "label": "Google AI Studio",
  "provider": "gemini",
  "credentials": {
    "apiKey": "AIzaSy..."
  },
  "models": [
    {
      "id": "gemini-2.5-pro",
      "label": "Gemini 2.5 Pro",
      "roles": ["conversational"],
      "params": { "temperature": 0.7, "thinking": true },
      "systemPrompt": "Prefer concise answers."
    },
    {
      "id": "gemini-2.5-flash",
      "label": "Gemini 2.5 Flash",
      "roles": ["flash"],
      "params": { "temperature": 0 }
    }
  ],
  "preferredModels": {
    "conversational": "gemini-2.5-pro",
    "flash": "gemini-2.5-flash"
  }
}
```

![The core AI settings panel: a default-model dropdown at the top,
then a list of configs - an OpenAI-compatible llama.cpp server
and a Google Gemini config - each with credentials and a model list with
per-model role checkboxes.](media/0029/llm-settings.png)

_Prototype: the settings panel - configs, their model catalogs with role
tags, and the default model selected in one place. (The screenshot predates
the move into core, so it shows the settings under a plugin heading; the
content is unchanged.)_

The **Selected/Default Model** is the active `Config:Model` for each role
type, owned by the core gateway as a user setting. It lives in the core
rather than the assistant precisely because the assistant is just one
consumer: most plugins
just ask for the 'insert-role-here' model and use that one, so the model used
throughout the UI can be changed for all LLM users in a single place.

## Provider implementation: build vs. library

The Provider layer (normalizing wire formats, tool-call translation, streaming,
error handling across LLM backends) is a significant amount of work to implement
from scratch (though AI coding assistants make light work of stuff like this).
Rather than rolling our own, we _could_ leverage an existing open-source
TypeScript library.

Options considered:

- **Vercel AI SDK** (`@ai-sdk/provider` + `@ai-sdk/provider-utils`)
  - Pros: Mature, well-maintained, formal `LanguageModelV4` spec for custom
    providers; built-in providers for OpenAI, Anthropic, Google, Cohere,
    Mistral, Groq, etc.; handles streaming, tool calling, structured outputs,
    prompt caching; strong TypeScript types; active community.
  - Cons: Adds dependencies; provider registration is static at initialisation
    (no built-in runtime plugin registration — would need a thin wrapper
    layer); React-oriented UI layer (but AI SDK Core is framework-agnostic).
- **llm-harness** (TypeScript, Node.js)
  - Pros: Thin router with built-in retries, circuit breakers, fallback
    chains, cost tracking; peer dependencies (lazy-loaded, small bundle);
    supports OpenAI, Anthropic, Google, Ollama, and any OpenAI-compatible
    endpoint.
  - Cons: Smaller community; fewer provider implementations out of the box;
    no built-in plugin registration mechanism.

**Decision: build, don't buy.** Two reasons settle it:

- **Supply-chain risk**: pulling an SDK (and its transitive dependency tree)
  into the UI is exactly the npm attack surface we'd rather not take on for a
  feature handling user credentials and trace data. Fewer third-party
  dependencies is the safer posture.
- **LLM-generated code makes it cheap**: a Provider is effectively a fairly
  mechanical wrapper around a `fetch` (wire-format translation, SSE parsing,
  error normalisation) - precisely the kind of bounded, well-specified code an
  AI coding assistant writes quickly and well. The cost that historically
  justified reaching for a library has largely evaporated.

Either way the Provider interface above is the contract; this just means each
Provider is our own code rather than a wrapper around a library, so the choice
doesn't leak into the rest of the design.

## Open questions

1. ~~**Build vs. library for the Provider layer**~~ - **Decided: build.** We'll
   implement each Provider ourselves rather than wrap an SDK (Vercel AI SDK,
   llm-harness), to avoid the npm supply-chain risk of extra dependencies and
   because LLM-generated code makes the mechanical Provider layer cheap to
   write. See "Provider implementation: build vs. library" above.
2. **Cancellation: per-turn signal vs. `Conversation.abort()`.** The sketch
   above threads an optional `AbortSignal` into each `sendTurn`. The alternative
   is an `abort()` method on the `Conversation` that cancels whatever request is
   currently in flight.
   - **Per-turn `AbortSignal`** (sketched): web-platform idiom, forwards
     straight to `fetch` (and to Chrome's Prompt API, which also takes a
     signal); scoped to exactly one turn so there's no "which request?"
     ambiguity or in-flight state to track; composable
     (`AbortSignal.any([userCancel, AbortSignal.timeout(n)])`); separates
     cleanly from `dispose()` (cancel a turn vs. end the conversation). Costs
     the caller a controller to hold - but the agent loop holds one anyway for
     the "stop the next iteration" half of cancel (see
     [RFC-0032](0032-embedded-assistant.md)).
   - **`Conversation.abort()`**: marginally more convenient (no controller to
     thread), but requires every provider to track its current request, is
     ambiguous between/around turns, doesn't compose with timeouts, and blurs
     into `dispose()`.

   Leaning per-turn signal; noted as open in case a provider surfaces that the
   signal model can't express.
3. **Where do default/recommended models come from - Provider (code) or Config
   (data)?** Today the model catalog and `preferredModels` live in the Config
   (a data blob, possibly pushed by an extension server). But some backends have
   a fixed, code-known catalog - Chrome's Prompt API exposes essentially one
   model with no `listModels` endpoint - and even for open catalogs we may want
   to be assertive in code about which models are worth using from each backend,
   updating that as new models ship. Options:
   - **Provider supplies built-in defaults**: each Provider ships a curated
     recommended/default model list (and preferred-per-role) in code. Required
     for fixed-catalog backends like Prompt API; lets us bless "the good model"
     per backend and bump it in a normal release. Config can still override.
   - **Config/data only**: catalog stays purely in Config (extension server /
     `embedders.ts` / user settings); the Provider only does dynamic
     `listModels`. Updatable without a UI release, but no curation and awkward
     for backends with no model list to enumerate.
   - **Hybrid**: Provider ships sensible code defaults; a Config (notably one
     pushed by an extension server) overrides when present. Probably where it
     lands - but it overlaps with the extension server's existing ability to
     push `preferredModels`, and the precedence between code defaults and
     server-pushed configs is unclear.

   Punt for now: prototype the Prompt-API and extension-server cases and see how
   the precedence shakes out before committing.
