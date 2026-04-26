DNS - my temporary scratchpad for this Vite cl

## Open (need decisions)

- [ ] **Sourcemaps strategy.** Currently disabled in dev (intentional
  for speed), enabled in prod via Vite's default. The old
  `embedMinimalSourceMap` rollup hack (minimal maps embedded as
  `self.__SOURCEMAPS[name]`, consumed at runtime by
  `ui/src/base/source_map_utils.ts`) was *not* ported. Decide whether
  to keep that scheme and add a Vite `generateBundle` plugin for it,
  or drop it in favor of standard `.map` files.

- [ ] **`isolatedModules` decision.** `histogram.ts` and
  `track_helper.ts` are back to mixed value/type re-exports. Prod and
  dev both build clean. Enabling `isolatedModules: true` in
  `tsconfig.base.json` for stricter checks would require putting the
  `import type` / `export type` annotations back. Decide if/when.

## Done (this CL)

- [x] Replaced `vite build --watch` for the frontend with Vite's dev
  server (ESM modules served on demand, ~2s reload after .ts edit).
- [x] Switched `tsconfig.base.json` to `module: esnext` +
  `moduleResolution: bundler`.
- [x] Wired `vite-plugin-checker` into the dev server (project-wide
  tsc --noEmit in a worker, terminal output + browser overlay).
- [x] Added `optimizeDeps: { entries: ['src/frontend/index.ts'] }` to
  the dev server config — suppresses the transient
  "node_modules/.vite/deps/chunk-XXXX.js does not exist" errors that
  fired when Vite re-optimized after discovering imports late.
  Verified: hammered `/` six times in parallel + Playwright load
  end-to-end with cold cache, zero "does not exist" warnings.
- [x] Smoke-tested `--bigtrace` and `--open-perfetto-trace` end-to-end
  with Playwright — both load without console errors.
- [x] Suppressed dev-mode "preloaded but not used within a few
  seconds" warnings by zeroing the `assetsPreload` map in the
  dev-server's HTML transform. The fonts / CSS still load on demand
  later, just without a preload hint.
- [x] Updated `docs/AGENTS-ui.md` with a "Build & dev architecture"
  section describing the new dev pipeline (Vite dev server for
  frontend, one-shot vs watch bundles, HTML transform, type checking
  via vite-plugin-checker).
- [x] Wrote `rfcs/0018-ui-vite-migration.md` documenting the move
  (problem, decision, design, alternatives considered).
- [x] **Cold full build** verified: with everything wiped
  (`out/ui/ui`, `out/ui/wasm{,_memory64}`, `ui/src/gen`),
  `ui/build --only-wasm-memory64` ran end-to-end in ~60s
  (`tools/install-build-deps --check-only --ui` → `tools/gn` →
  `tools/ninja` 3467 steps with ccache → codegen → vite builds → SW
  manifest). Exit 0, dist tree matches the rollup-era layout.

