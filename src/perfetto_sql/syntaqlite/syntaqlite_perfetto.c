/*
** syntaqlite amalgamation — machine generated, do not edit.
*/

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wswitch-enum"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wunused-macros"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wunreachable-code"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wswitch-default"
#pragma GCC diagnostic ignored "-Wpadded"
#ifndef __cplusplus
#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#else
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wextra-semi-stmt"
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wmissing-variable-declarations"
#pragma clang diagnostic ignored "-Wimplicit-int-conversion"
#pragma clang diagnostic ignored "-Wimplicit-int-enum-cast"
#pragma clang diagnostic ignored "-Wimplicit-void-ptr-cast"
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#elif defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic ignored "-Wold-style-declaration"
#endif
#endif

#ifndef SYNTAQLITE_OMIT_SQLITE_API
#define SYNTAQLITE_OMIT_SQLITE_API
#endif

#include "syntaqlite_perfetto.h"

/* ======== begin: syntaqlite_dialect/vec.h ======== */
#ifndef SYNTAQLITE_EXT_VEC_H
#define SYNTAQLITE_EXT_VEC_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Header-only type-generic dynamic array.
// Works on any struct with {T *data; uint32_t count; uint32_t capacity;}.
//
// All mutating operations take a SyntaqliteMemMethods parameter for
// allocation. This lets the vec use the caller's configured allocator.


#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// Type constructor
#define SYNQ_VEC(T)    \
  struct {             \
    T* data;           \
    uint32_t count;    \
    uint32_t capacity; \
  }

// Zero-init
#define syntaqlite_vec_init(v) \
  do {                         \
    (v)->data = NULL;          \
    (v)->count = 0;            \
    (v)->capacity = 0;         \
  } while (0)

// Free + zero
#define syntaqlite_vec_free(v, mem) \
  do {                              \
    (mem).xFree((v)->data);         \
    (v)->data = NULL;               \
    (v)->count = 0;                 \
    (v)->capacity = 0;              \
  } while (0)

// Reset count, keep allocation
#define syntaqlite_vec_clear(v) \
  do {                          \
    (v)->count = 0;             \
  } while (0)

// Ensure capacity >= needed (capacity is always a power of two).
#define syntaqlite_vec_ensure(v, needed, mem)             \
  do {                                                    \
    if ((needed) > (v)->capacity) {                       \
      uint32_t _cap = (v)->capacity ? (v)->capacity : 16; \
      while (_cap < (needed))                             \
        _cap *= 2;                                        \
      (v)->data = (__typeof__((v)->data))(mem).xRealloc(  \
          (v)->data, (size_t)_cap * sizeof(*(v)->data));  \
      (v)->capacity = _cap;                               \
    }                                                     \
  } while (0)

// Append one element, grow if needed
#define syntaqlite_vec_push(v, val, mem)             \
  do {                                               \
    syntaqlite_vec_ensure((v), (v)->count + 1, mem); \
    (v)->data[(v)->count++] = (val);                 \
  } while (0)

// Element count
#define syntaqlite_vec_len(v) ((v)->count)

// Lvalue access to element at index
#define syntaqlite_vec_at(v, i) ((v)->data[i])

// Set count to n, discarding trailing elements
#define syntaqlite_vec_truncate(v, n) \
  do {                                \
    (v)->count = (n);                 \
  } while (0)

// Decrement count, evaluate to last element
#define syntaqlite_vec_pop(v) ((v)->data[--(v)->count])

// Bulk append via memcpy
#define syntaqlite_vec_push_n(v, src, n, mem)                               \
  do {                                                                      \
    uint32_t _n = (n);                                                      \
    syntaqlite_vec_ensure((v), (v)->count + _n, mem);                       \
    memcpy((v)->data + (v)->count, (src), (size_t)_n * sizeof(*(v)->data)); \
    (v)->count += _n;                                                       \
  } while (0)

#ifdef __cplusplus
}
#endif


#endif  /* SYNTAQLITE_EXT_VEC_H */
/* ======== end: syntaqlite_dialect/vec.h ======== */

/* ======== begin: syntaqlite_dialect/arena.h ======== */
#ifndef SYNTAQLITE_EXT_ARENA_H
#define SYNTAQLITE_EXT_ARENA_H

// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Arena allocator with offset table for node-based data structures.


#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

typedef struct SynqArena {
  SYNQ_VEC(uint8_t) data;
  SYNQ_VEC(uint32_t) offsets;
} SynqArena;

// Get pointer to node data by offset-table ID.
#define synq_arena_ptr(a, id) \
  (&syntaqlite_vec_at(&(a)->data, syntaqlite_vec_at(&(a)->offsets, id)))

// Const-correct variant for read-only access.
#define synq_arena_cptr(a, id) ((const uint8_t*)synq_arena_ptr((a), (id)))

static inline void synq_arena_init(SynqArena* a) {
  syntaqlite_vec_init(&a->data);
  syntaqlite_vec_init(&a->offsets);
}

static inline void synq_arena_free(SynqArena* a, SyntaqliteMemMethods mem) {
  syntaqlite_vec_free(&a->data, mem);
  syntaqlite_vec_free(&a->offsets, mem);
}

// Reset counts to zero, keeping allocated memory for reuse.
static inline void synq_arena_clear(SynqArena* a) {
  syntaqlite_vec_clear(&a->data);
  syntaqlite_vec_clear(&a->offsets);
}

// Copy data into the arena and register in the offset table.
// Returns the node ID.
static inline uint32_t synq_arena_alloc(SynqArena* a,
                                        const void* data,
                                        uint32_t size,
                                        SyntaqliteMemMethods mem) {
  uint32_t node_id = syntaqlite_vec_len(&a->offsets);
  syntaqlite_vec_push(&a->offsets, syntaqlite_vec_len(&a->data), mem);
  syntaqlite_vec_push_n(&a->data, data, size, mem);
  return node_id;
}

// Reserve a node ID in the offset table without allocating arena bytes.
// The offset is written later by synq_arena_commit.
static inline uint32_t synq_arena_reserve_id(SynqArena* a,
                                             SyntaqliteMemMethods mem) {
  uint32_t node_id = syntaqlite_vec_len(&a->offsets);
  syntaqlite_vec_push(&a->offsets, 0, mem);
  return node_id;
}

// Commit data at a previously reserved node ID.
static inline void synq_arena_commit(SynqArena* a,
                                     uint32_t node_id,
                                     const void* data,
                                     uint32_t size,
                                     SyntaqliteMemMethods mem) {
  syntaqlite_vec_at(&a->offsets, node_id) = syntaqlite_vec_len(&a->data);
  syntaqlite_vec_push_n(&a->data, data, size, mem);
}

// Append raw bytes to the arena without registering an offset entry.
static inline void synq_arena_append(SynqArena* a,
                                     const void* data,
                                     uint32_t size,
                                     SyntaqliteMemMethods mem) {
  syntaqlite_vec_push_n(&a->data, data, size, mem);
}

#ifdef __cplusplus
}
#endif


#endif  /* SYNTAQLITE_EXT_ARENA_H */
/* ======== end: syntaqlite_dialect/arena.h ======== */

/* ======== begin: syntaqlite_dialect/dialect_abi.h ======== */
#ifndef SYNTAQLITE_INTERNAL_DIALECT_ABI_H
#define SYNTAQLITE_INTERNAL_DIALECT_ABI_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Runtime ↔ dialect ABI contract.
//
// Dialects can be linked into the runtime statically (amalgamation builds,
// cargo-built native binaries) or loaded as separately-compiled side modules
// (emscripten MAIN_MODULE=2 / SIDE_MODULE=1, future `dlopen` support). In the
// latter case, neither half of the link sees the other's symbols at compile
// time, and dead-code elimination will drop any symbol that isn't explicitly
// marked as part of the cross-module surface.
//
// Two kinds of calls cross this boundary:
//
//   dialect → runtime
//     The generated grammar action code and Lemon-emitted parser call back
//     into runtime-owned helpers during reduce/shift. Everything exported
//     here lives in `syntaqlite-syntax/csrc/parser_extents.c` or similar
//     runtime-side C files. Current members:
//       - synq_extent_on_shift   (see syntaqlite_dialect/extent_hooks.h)
//       - synq_extent_on_reduce  (see syntaqlite_dialect/extent_hooks.h)
//
//   runtime → dialect
//     The runtime drives each dialect through a `SyntaqliteDialectTemplate`
//     whose function pointers reference symbols emitted by the dialect's
//     generated parser/tokenizer. Current members (per dialect, Pascal-cased):
//       - Synq<Dialect>ParseAlloc / ParseInit / ParseFinalize / ParseFree
//       - Synq<Dialect>Parse
//       - Synq<Dialect>ParseTrace
//       - Synq<Dialect>ParseExpectedTokens
//       - Synq<Dialect>ParseCompletionContext
//       - Synq<Dialect>ParseFallback
//       - Synq<Dialect>GetToken
//
// Every declaration in that set must be tagged with `SYNTAQLITE_DIALECT_API`
// so it survives dead-code elimination and is visible across module
// boundaries. Renaming or changing the signature of any symbol above is an
// ABI break: update both sides in lockstep.


// `used` keeps wasm-ld / LTO from discarding the symbol when the main module
// has no direct reference to it (the referencing side module is linked
// separately). `visibility("default")` ensures it ends up in the module's
// export table rather than being internal.
#if defined(__GNUC__) || defined(__clang__)
#define SYNTAQLITE_DIALECT_API __attribute__((used, visibility("default")))
#else
#define SYNTAQLITE_DIALECT_API
#endif


#endif  /* SYNTAQLITE_INTERNAL_DIALECT_ABI_H */
/* ======== end: syntaqlite_dialect/dialect_abi.h ======== */

/* ======== begin: syntaqlite_dialect/ast_builder.h ======== */
#ifndef SYNTAQLITE_EXT_AST_BUILDER_H
#define SYNTAQLITE_EXT_AST_BUILDER_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Parse context and AST builder interface.
// Provides:
//   - SynqParseCtx: parse/AST state threaded via %extra_argument
//   - SynqParseToken: terminal token type (used as %token_type in lemon
//   grammar)
//   - synq_span(): converts SynqParseToken to SyntaqliteTextSpan
//   - AST builder functions: synq_parse_build, synq_parse_list_append, etc.
//   - AST_NODE macro for in-place AST node mutation
//
// Grammar actions receive pCtx via lemon's %extra_argument mechanism.


#include <stdint.h>
#include <string.h>


#define SYNQ_NO_SPAN ((SyntaqliteTextSpan){0})

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// List descriptor: lightweight metadata for one in-progress list.
// ---------------------------------------------------------------------------

typedef struct SynqListDesc {
  uint32_t node_id;  // reserved arena ID
  uint32_t offset;   // start index into child_buf
  uint32_t tag;
} SynqListDesc;

// Per-node extent: half-open byte range in the authored source plus an
// inclusive token-index range into `p->tokens`.  Both ranges are
// maintained by the extent hooks on one shadow stack and merged
// together on reduce.
//
// Sentinels:
//   `root_start == UINT32_MAX && root_end == 0` — no byte range recorded
//       (pure epsilon reduction, neutral under min/max merging).
//   `first_tok == UINT32_MAX` — no tokens recorded (layer-N shift with
//       UINT32_MAX token_idx, or pure epsilon).  `last_tok` is then
//       also UINT32_MAX.
//
// The two sentinels are independent: a macro-expansion-only node can
// have a valid byte range (the call site's root coordinates) but no
// layer-0 tokens — or have tokens in `p->tokens` after the token-stream
// unification.  Consumers check the specific sentinel they care about.
typedef struct SynqExtentRange {
  uint32_t root_start;
  uint32_t root_end;
  uint32_t first_tok;
  uint32_t last_tok;
} SynqExtentRange;

// Layer-local byte range used by per-node *expanded*-text tracking —
// the bytes the tokenizer saw for a node, in whichever layer buffer
// they live.
//
// Sentinel states:
//   {0, 0, 0}                  — epsilon (no tokens); neutral in merges.
//   {_, 0, SYNQ_CROSS_LAYER}   — cross-layer poison; propagates through
//                                parent merges so the fast path falls
//                                through to the slow path.
#define SYNQ_CROSS_LAYER UINT32_MAX
typedef struct SynqNodeExpandedExtent {
  uint32_t offset;
  uint32_t length;
  uint32_t layer_id;
} SynqNodeExpandedExtent;

// Straddle stack entry values (packed into uint32_t):
//   0              = source terminal
//   1..N           = terminal from outermost expansion layer N
//   SYNQ_STRADDLE_NEUTRAL = non-terminal / epsilon (neutral in checks)
#define SYNQ_STRADDLE_NEUTRAL UINT32_MAX

// ---------------------------------------------------------------------------
// Parse context — threaded through grammar actions via %extra_argument
// ---------------------------------------------------------------------------

typedef struct SynqParseCtx {
  // AST storage
  SyntaqliteMemMethods mem;
  SynqArena ast;
  SYNQ_VEC(uint32_t) child_buf;
  SYNQ_VEC(SynqListDesc) list_stack;

  // Parser state
  const char* source;  // Source text base pointer (for offset computation).
  const SyntaqliteDialect* env;   // Dialect env (for cflag checks in actions).
  uint32_t root;                  // Root node ID of the current statement.
  uint32_t stmt_completed;        // Set by grammar actions when ecmd reduces.
  uint32_t pending_explain_mode;  // 1=EXPLAIN, 2=EXPLAIN QUERY PLAN (set by
                                  // explain rule, consumed by cmdx ::= cmd).
  uint32_t error;                 // Set when a syntax error occurs.
  uint32_t error_offset;          // Byte offset of the error token in source.
  uint32_t error_length;          // Byte length of the error token.
  uint32_t saw_subquery;  // Set by grammar actions when a subquery is reduced.
  uint32_t saw_update_delete_limit;  // Set when ORDER BY / LIMIT used on DELETE
                                     // or UPDATE.

  // Token marking — points to the parser's token list (NULL if not collecting).
  // Typed as void* because SYNQ_VEC produces anonymous struct types; the
  // synq_mark_as_id() helper casts it to the right layout.
  void* tokens;

  // Expansion layer index for span construction.
  // 0 = original source, 1+ = index into the layer tree (1-based).
  uint32_t layer_id;

  // Counter for "currently parsing inside a macro definition body".
  // While > 0, the tokenizer skips macro expansion so the body is captured
  // verbatim instead of being recursively expanded.  Set/cleared by
  // grammar actions on entering/leaving the body production.
  uint32_t in_macro_def_body;

  // Byte offset of the token Lemon is currently processing (in
  // root-source coordinates).  Set at the start of
  // `synq_parser_shift_token` *before* `SYNQ_PARSER_FEED` runs, so
  // empty-rule reductions firing inside the feed observe the offset of
  // the token they're about to be shifted alongside.  BEFORE-style
  // markers use this to capture the start position of a non-terminal
  // (whitespace before the first terminal is excluded).  Valid only
  // for tokens shifted from the root source layer.
  uint32_t cur_shift_start;

  // Byte offset just past the end of the most recently shifted terminal
  // (in root-source coordinates).  Updated in
  // `synq_parser_shift_token` *after* `SYNQ_PARSER_FEED` returns, so
  // that empty-rule reductions firing inside the feed see the end of
  // the *previous* shifted terminal, not the current one.  AFTER-style
  // markers use this to capture the end position of a non-terminal.
  // Valid only for tokens shifted from the root source layer.
  uint32_t last_shifted_end;

  // Per-node extent tracking.  Opt-in via `collect_node_extents`.
  // `extent_stack` / `node_extents` track the merged *authored*
  // source range (in root coordinates) — used by
  // `syntaqlite_parser_node_text`.  `expanded_stack` /
  // `node_expanded_extents` track the merged *expanded* range in the
  // tokens' own layer — used by
  // `syntaqlite_parser_node_expanded_text`; mixed-layer merges
  // collapse to a sentinel.  `macro_root_*` caches the outermost
  // currently-active macro call site in root coordinates so tokens
  // shifted inside expansions can be attributed back to the authored
  // source.
  SYNQ_VEC(SynqExtentRange) extent_stack;
  SYNQ_VEC(SynqExtentRange) node_extents;
  SYNQ_VEC(SynqNodeExpandedExtent) expanded_stack;
  SYNQ_VEC(SynqNodeExpandedExtent) node_expanded_extents;
  uint32_t collect_node_extents;
  uint32_t macro_root_start;
  uint32_t macro_root_end;
  uint32_t macro_root_layer;    // Outermost expansion layer idx (set on entry).
  uint32_t has_macro_straddle;  // Sticky flag set during reduce.
  uint32_t lemon_depth;  // Lemon stack depth (always tracked, for lazy init).
  SYNQ_VEC(uint32_t) straddle_stack;  // Lazily initialized on first macro use.
  // Set when `GENERATED ALWAYS` was trimmed off a column's type name, so the
  // `AS` production can still emit the keywords.
  uint32_t generated_always;
} SynqParseCtx;

// Common header for all list nodes in the arena.
typedef struct SynqListHeader {
  uint32_t tag;
  uint32_t count;
} SynqListHeader;

// ---------------------------------------------------------------------------
// AST node access macro (for in-place mutation in grammar actions)
// ---------------------------------------------------------------------------

// Cast the arena pointer for a node ID to a void pointer.
// Dialect code should further cast to the dialect-specific node union.
#define AST_NODE(arena_ptr, id) ((void*)synq_arena_ptr((arena_ptr), (id)))

// Type-safe arena access — casts through void* to suppress -Wcast-align.
#define AST_NODE_AS(type, arena_ptr, id) \
  ((type*)((void*)synq_arena_ptr((arena_ptr), (id))))

// ---------------------------------------------------------------------------
// AST builder functions
// ---------------------------------------------------------------------------

// Flush the topmost list from the stack into the arena.
static inline void synq_parse_list_flush_top(SynqParseCtx* ctx) {
  SynqListDesc* desc = &syntaqlite_vec_at(
      &ctx->list_stack, syntaqlite_vec_len(&ctx->list_stack) - 1);
  uint32_t count = syntaqlite_vec_len(&ctx->child_buf) - desc->offset;
  uint32_t children_size = count * (uint32_t)sizeof(uint32_t);

  SynqListHeader hdr = {.tag = desc->tag, .count = count};
  synq_arena_commit(&ctx->ast, desc->node_id, &hdr, (uint32_t)sizeof(hdr),
                    ctx->mem);
  synq_arena_append(&ctx->ast,
                    &syntaqlite_vec_at(&ctx->child_buf, desc->offset),
                    children_size, ctx->mem);

  syntaqlite_vec_truncate(&ctx->child_buf, desc->offset);
  (void)syntaqlite_vec_pop(&ctx->list_stack);
}

static inline void synq_parse_ctx_init(SynqParseCtx* ctx,
                                       SyntaqliteMemMethods mem) {
  ctx->mem = mem;
  synq_arena_init(&ctx->ast);
  syntaqlite_vec_init(&ctx->child_buf);
  syntaqlite_vec_init(&ctx->list_stack);
  syntaqlite_vec_init(&ctx->extent_stack);
  syntaqlite_vec_init(&ctx->node_extents);
  syntaqlite_vec_init(&ctx->expanded_stack);
  syntaqlite_vec_init(&ctx->node_expanded_extents);
  ctx->collect_node_extents = 0;
  ctx->macro_root_start = 0;
  ctx->macro_root_end = 0;
  ctx->macro_root_layer = 0;
  ctx->has_macro_straddle = 0;
  ctx->lemon_depth = 0;
  syntaqlite_vec_init(&ctx->straddle_stack);
  ctx->generated_always = 0;
}

static inline void synq_parse_ctx_free(SynqParseCtx* ctx) {
  syntaqlite_vec_free(&ctx->child_buf, ctx->mem);
  syntaqlite_vec_free(&ctx->list_stack, ctx->mem);
  syntaqlite_vec_free(&ctx->extent_stack, ctx->mem);
  syntaqlite_vec_free(&ctx->node_extents, ctx->mem);
  syntaqlite_vec_free(&ctx->expanded_stack, ctx->mem);
  syntaqlite_vec_free(&ctx->node_expanded_extents, ctx->mem);
  syntaqlite_vec_free(&ctx->straddle_stack, ctx->mem);
  synq_arena_free(&ctx->ast, ctx->mem);
}

// Reset to empty state, keeping allocated memory for reuse.
static inline void synq_parse_ctx_clear(SynqParseCtx* ctx) {
  syntaqlite_vec_clear(&ctx->child_buf);
  syntaqlite_vec_clear(&ctx->list_stack);
  syntaqlite_vec_clear(&ctx->extent_stack);
  syntaqlite_vec_clear(&ctx->node_extents);
  syntaqlite_vec_clear(&ctx->expanded_stack);
  syntaqlite_vec_clear(&ctx->node_expanded_extents);
  synq_arena_clear(&ctx->ast);
  ctx->macro_root_start = 0;
  ctx->macro_root_end = 0;
  ctx->macro_root_layer = 0;
  ctx->has_macro_straddle = 0;
  ctx->lemon_depth = 0;
  syntaqlite_vec_clear(&ctx->straddle_stack);
}

// Record the current shadow-stack tops (authored + expanded) as the
// extents for `node_id`.  Called right after a node is allocated, so
// the tops are the merged ranges for the rule currently being
// reduced.  List node ids are re-recorded on each append, so the
// final stored value is the full list's extent.  Node ids are
// monotonically allocated, so `node_id` is either equal to
// `node_extents.count` (new) or less (existing).
static inline void synq_extent_record(SynqParseCtx* ctx, uint32_t node_id) {
  if (!ctx->collect_node_extents) {
    return;
  }
  uint32_t stack_len = syntaqlite_vec_len(&ctx->extent_stack);
  if (stack_len == 0) {
    return;
  }
  SynqExtentRange top = syntaqlite_vec_at(&ctx->extent_stack, stack_len - 1);
  SynqNodeExpandedExtent exp_top =
      syntaqlite_vec_at(&ctx->expanded_stack, stack_len - 1);
  if (node_id < ctx->node_extents.count) {
    syntaqlite_vec_at(&ctx->node_extents, node_id) = top;
    syntaqlite_vec_at(&ctx->node_expanded_extents, node_id) = exp_top;
  } else {
    syntaqlite_vec_push(&ctx->node_extents, top, ctx->mem);
    syntaqlite_vec_push(&ctx->node_expanded_extents, exp_top, ctx->mem);
  }
}

// Re-record the current shadow-stack extent for an existing node ID.
// Use this in multi-RHS grammar rules that pass through a child node ID
// without allocating a new node (e.g. `A = B;`).  Without this call,
// node_extents[child] retains the child's original (narrower) range
// instead of the merged range of the enclosing rule.
static inline uint32_t synq_pass(SynqParseCtx* ctx, uint32_t child) {
  synq_extent_record(ctx, child);
  return child;
}

// Generic node builder: copy node data into the arena.
static inline uint32_t synq_parse_build(SynqParseCtx* ctx,
                                        const void* node_data,
                                        uint32_t node_size) {
  uint32_t node_id =
      synq_arena_alloc(&ctx->ast, node_data, node_size, ctx->mem);
  synq_extent_record(ctx, node_id);
  return node_id;
}

// Record a list's extent as the union of its members, instead of the current
// grammar reduction. This is useful for semantic sublists assembled inside a
// larger left-recursive reduction. Expanded-layer merging includes every child.
SYNTAQLITE_DIALECT_API void synq_extent_record_list_append(SynqParseCtx* ctx,
                                                           uint32_t list_id,
                                                           uint32_t child);

static inline uint32_t synq_parse_list_append_impl(SynqParseCtx* ctx,
                                                   uint32_t tag,
                                                   uint32_t list_id,
                                                   uint32_t child,
                                                   int child_extents) {
  if (list_id == SYNTAQLITE_NULL_NODE) {
    SynqListDesc desc;
    desc.node_id = synq_arena_reserve_id(&ctx->ast, ctx->mem);
    desc.offset = syntaqlite_vec_len(&ctx->child_buf);
    desc.tag = tag;
    syntaqlite_vec_push(&ctx->list_stack, desc, ctx->mem);
    syntaqlite_vec_push(&ctx->child_buf, child, ctx->mem);
    if (child_extents) {
      if (ctx->collect_node_extents)
        synq_extent_record_list_append(ctx, desc.node_id, child);
    } else
      synq_extent_record(ctx, desc.node_id);
    return desc.node_id;
  }

  // Auto-flush completed inner lists above the target.
  while (syntaqlite_vec_at(&ctx->list_stack,
                           syntaqlite_vec_len(&ctx->list_stack) - 1)
             .node_id != list_id) {
    synq_parse_list_flush_top(ctx);
  }
  syntaqlite_vec_push(&ctx->child_buf, child, ctx->mem);
  if (child_extents) {
    if (ctx->collect_node_extents)
      synq_extent_record_list_append(ctx, list_id, child);
  } else
    synq_extent_record(ctx, list_id);
  return list_id;
}

static inline uint32_t synq_parse_list_append(SynqParseCtx* ctx,
                                              uint32_t tag,
                                              uint32_t list_id,
                                              uint32_t child) {
  return synq_parse_list_append_impl(ctx, tag, list_id, child, 0);
}

static inline uint32_t synq_parse_list_append_from_children(SynqParseCtx* ctx,
                                                            uint32_t tag,
                                                            uint32_t list_id,
                                                            uint32_t child) {
  return synq_parse_list_append_impl(ctx, tag, list_id, child, 1);
}

// Like list_append, but inserts the child at the front of the list.
// Used for right-recursive grammar rules where the innermost (last in source)
// clause reduces first, so each outer clause must prepend to maintain source
// order.
static inline uint32_t synq_parse_list_prepend(SynqParseCtx* ctx,
                                               uint32_t tag,
                                               uint32_t list_id,
                                               uint32_t child) {
  if (list_id == SYNTAQLITE_NULL_NODE) {
    return synq_parse_list_append(ctx, tag, list_id, child);
  }

  // Auto-flush completed inner lists above the target.
  while (syntaqlite_vec_at(&ctx->list_stack,
                           syntaqlite_vec_len(&ctx->list_stack) - 1)
             .node_id != list_id) {
    synq_parse_list_flush_top(ctx);
  }

  // Find the list descriptor to get its start offset.
  SynqListDesc* desc = &syntaqlite_vec_at(
      &ctx->list_stack, syntaqlite_vec_len(&ctx->list_stack) - 1);
  uint32_t insert_at = desc->offset;
  uint32_t len = syntaqlite_vec_len(&ctx->child_buf);

  // Make room: push a dummy, shift elements right, insert at front.
  syntaqlite_vec_push(&ctx->child_buf, child, ctx->mem);
  for (uint32_t i = len; i > insert_at; --i) {
    syntaqlite_vec_at(&ctx->child_buf, i) =
        syntaqlite_vec_at(&ctx->child_buf, i - 1);
  }
  syntaqlite_vec_at(&ctx->child_buf, insert_at) = child;
  synq_extent_record(ctx, list_id);
  return list_id;
}

static inline void synq_parse_list_flush(SynqParseCtx* ctx) {
  while (syntaqlite_vec_len(&ctx->list_stack) > 0) {
    synq_parse_list_flush_top(ctx);
  }
}

// ---------------------------------------------------------------------------
// Token → span conversion
// ---------------------------------------------------------------------------

static inline SyntaqliteTextSpan synq_span(SynqParseCtx* ctx,
                                           SynqParseToken tok) {
  (void)ctx;
  if (tok.z == NULL)
    return (SyntaqliteTextSpan){0};
  return (SyntaqliteTextSpan){
      .offset = tok.offset,
      .length = tok.n,
      .flags = 0,
      ._layer_id = tok.layer_id,
  };
}

// Like synq_span() but strips surrounding quote characters from quoted
// identifiers, matching SQLite's tokenExpr() dequoting behavior.
// Handles "...", `...`, [...], and '...' forms — every call site is an
// identifier position, so a STRING token here is a string literal used
// as an identifier (`nm ::= STRING`) and dequotes like the rest.  For
// unquoted tokens, equivalent to synq_span().  Records which quote
// character bracketed the identifier in the span flags so consumers can
// recover the original quote style.
//
// The span stays a zero-copy slice: a doubled quote char inside the
// token (SQLite's escape, e.g. `'a''b'` for `a'b`) survives verbatim in
// the inner text.  Consumers that need the identifier *value* collapse
// it with `unescape_quoted` (Rust) keyed off the span's quote flag.
static inline SyntaqliteTextSpan synq_span_dequote(SynqParseCtx* ctx,
                                                   SynqParseToken tok) {
  (void)ctx;
  if (tok.z == NULL)
    return (SyntaqliteTextSpan){0};
  if (tok.n >= 2) {
    char expected_close = 0;
    uint32_t kind_flag = 0;
    switch (tok.z[0]) {
      case '"':
        expected_close = '"';
        kind_flag = SYNTAQLITE_SPAN_FLAG_QUOTE_DOUBLE;
        break;
      case '`':
        expected_close = '`';
        kind_flag = SYNTAQLITE_SPAN_FLAG_QUOTE_BACKTICK;
        break;
      case '[':
        expected_close = ']';
        kind_flag = SYNTAQLITE_SPAN_FLAG_QUOTE_BRACKET;
        break;
      case '\'':
        expected_close = '\'';
        kind_flag = SYNTAQLITE_SPAN_FLAG_QUOTE_SINGLE;
        break;
      default:
        break;
    }
    if (kind_flag != 0 && tok.z[tok.n - 1] == expected_close) {
      SyntaqliteTextSpan sp = {
          .offset = tok.offset + 1,
          .length = tok.n - 2,
          .flags = kind_flag,
          ._layer_id = tok.layer_id,
      };
      return sp;
    }
  }
  return (SyntaqliteTextSpan){
      .offset = tok.offset,
      .length = tok.n,
      ._layer_id = tok.layer_id,
  };
}

// Mark a token as "used as identifier" (fallback from keyword).
// O(1) — uses the token_idx stored in SynqParseToken at collection time.
static inline void synq_mark_as_id(SynqParseCtx* ctx, SynqParseToken tok) {
  if (!ctx->tokens || tok.token_idx == 0xFFFFFFFF)
    return;
  // ctx->tokens is a void* pointing to SYNQ_VEC(SyntaqliteParserToken).
  // The vec layout is: { SyntaqliteParserToken* data; uint32_t count; uint32_t
  // capacity; }
  typedef struct {
    SyntaqliteParserToken* data;
    uint32_t count;
    uint32_t capacity;
  } TokenVec;
  TokenVec* tv = (TokenVec*)ctx->tokens;
  tv->data[tok.token_idx].flags |= SYNQ_TOKEN_FLAG_AS_ID;
}

// Mark a token as "used as function name" in a function-call expression.
// O(1) — uses the token_idx stored in SynqParseToken at collection time.
static inline void synq_mark_as_function(SynqParseCtx* ctx,
                                         SynqParseToken tok) {
  if (!ctx->tokens || tok.token_idx == 0xFFFFFFFF)
    return;
  // ctx->tokens is a void* pointing to SYNQ_VEC(SyntaqliteParserToken).
  // The vec layout is: { SyntaqliteParserToken* data; uint32_t count; uint32_t
  // capacity; }
  typedef struct {
    SyntaqliteParserToken* data;
    uint32_t count;
    uint32_t capacity;
  } TokenVec;
  TokenVec* tv = (TokenVec*)ctx->tokens;
  tv->data[tok.token_idx].flags |= SYNQ_TOKEN_FLAG_AS_FUNCTION;
}

// Mark a token as "used as type name" in type contexts.
// O(1) — uses the token_idx stored in SynqParseToken at collection time.
static inline void synq_mark_as_type(SynqParseCtx* ctx, SynqParseToken tok) {
  if (!ctx->tokens || tok.token_idx == 0xFFFFFFFF)
    return;
  // ctx->tokens is a void* pointing to SYNQ_VEC(SyntaqliteParserToken).
  // The vec layout is: { SyntaqliteParserToken* data; uint32_t count; uint32_t
  // capacity; }
  typedef struct {
    SyntaqliteParserToken* data;
    uint32_t count;
    uint32_t capacity;
  } TokenVec;
  TokenVec* tv = (TokenVec*)ctx->tokens;
  tv->data[tok.token_idx].flags |= SYNQ_TOKEN_FLAG_AS_TYPE;
}

// Range field metadata types (SyntaqliteFieldRangeMeta,
// SyntaqliteRangeMetaEntry) are defined in syntaqlite/dialect.h.

#ifdef __cplusplus
}
#endif


#endif  /* SYNTAQLITE_EXT_AST_BUILDER_H */
/* ======== end: syntaqlite_dialect/ast_builder.h ======== */

/* ======== begin: csrc/sqlite_parse.h ======== */
#ifndef SYNTAQLITE_PERFETTO_PARSE_H
#define SYNTAQLITE_PERFETTO_PARSE_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT


#include <stddef.h>
#include <stdint.h>
#include <stdio.h>


#ifdef __cplusplus
extern "C" {
#endif

SYNTAQLITE_DIALECT_API void* SynqPerfettoParseAlloc(void* (*mallocProc)(size_t), SynqParseCtx* pCtx);
SYNTAQLITE_DIALECT_API void SynqPerfettoParseInit(void* parser, SynqParseCtx* pCtx);
SYNTAQLITE_DIALECT_API void SynqPerfettoParseFinalize(void* parser);
SYNTAQLITE_DIALECT_API void SynqPerfettoParseFree(void* parser, void (*freeProc)(void*));
SYNTAQLITE_DIALECT_API void SynqPerfettoParse(void* parser, int token_type, SynqParseToken minor);
SYNTAQLITE_DIALECT_API uint32_t SynqPerfettoParseExpectedTokens(void* parser, uint32_t* out_tokens, uint32_t out_cap);
SYNTAQLITE_DIALECT_API uint32_t SynqPerfettoParseCompletionContext(void* parser);
SYNTAQLITE_DIALECT_API int SynqPerfettoParseFallback(int iToken);
#ifndef NDEBUG
SYNTAQLITE_DIALECT_API void SynqPerfettoParseTrace(FILE* trace_file, char* prompt);
#endif

#ifdef __cplusplus
}
#endif


#endif  /* SYNTAQLITE_PERFETTO_PARSE_H */
/* ======== end: csrc/sqlite_parse.h ======== */

/* ======== begin: syntaqlite_dialect/sqlite_compat.h ======== */
#ifndef SYNTAQLITE_INTERNAL_SQLITE_COMPAT_H
#define SYNTAQLITE_INTERNAL_SQLITE_COMPAT_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// SQLite compatibility definitions for syntaqlite.
// Provides type aliases, macros, and structures needed by tokenizer and parser.


#include <stdint.h>

// SQLite type aliases
typedef int64_t i64;
typedef uint8_t u8;
typedef uint32_t u32;

// Default to SQLITE_ASCII if not defined and SQLITE_EBCDIC also not defined
#if !defined(SQLITE_ASCII) && !defined(SQLITE_EBCDIC)
#define SQLITE_ASCII 1
#endif

// Default digit separator for numeric literals (e.g., 1_000_000)
#ifndef SQLITE_DIGIT_SEPARATOR
#define SQLITE_DIGIT_SEPARATOR '_'
#endif

// Case-insensitive comparison over ASCII, which is all SQL keywords need.
// Deliberately not libc's strncasecmp: the generated parser is compiled into
// embeddings that do not necessarily provide it, such as an Emscripten side
// module, where the unresolved import aborts the engine mid-parse.
#include <stddef.h>

static inline int synq_strncasecmp_ascii(const char* a,
                                         const char* b,
                                         size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned char ca = (unsigned char)a[i];
    unsigned char cb = (unsigned char)b[i];
    if (ca >= 'A' && ca <= 'Z') {
      ca = (unsigned char)(ca - 'A' + 'a');
    }
    if (cb >= 'A' && cb <= 'Z') {
      cb = (unsigned char)(cb - 'A' + 'a');
    }
    if (ca != cb) {
      return (int)ca - (int)cb;
    }
    if (ca == 0) {
      return 0;
    }
  }
  return 0;
}

#define SYNQ_STRNCASECMP synq_strncasecmp_ascii

// C++17 fallthrough, C no-op
#ifdef __cplusplus
#define deliberate_fall_through [[fallthrough]]
#else
#define deliberate_fall_through
#endif  // SYNTAQLITE_INTERNAL_SQLITE_COMPAT_H

// No-op for testcase macro if not defined
#ifndef testcase
#define testcase(X)
#endif

// No-op for assert macro if not defined
#ifndef assert
#define assert(X)
#endif


#endif  /* SYNTAQLITE_INTERNAL_SQLITE_COMPAT_H */
/* ======== end: syntaqlite_dialect/sqlite_compat.h ======== */

/* ======== begin: csrc/sqlite_tokenize.h ======== */
#ifndef SYNTAQLITE_INTERNAL_PERFETTO_TOKENIZE_H
#define SYNTAQLITE_INTERNAL_PERFETTO_TOKENIZE_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT



SYNTAQLITE_DIALECT_API i64 SynqPerfettoGetToken(const SyntaqliteDialect* env, const unsigned char* z, int* tokenType);


#endif  /* SYNTAQLITE_INTERNAL_PERFETTO_TOKENIZE_H */
/* ======== end: csrc/sqlite_tokenize.h ======== */

// Inline-dispatch macros for the perfetto dialect.
#if !defined(SYNTAQLITE_NO_INLINE_DIALECT_DISPATCH) && !defined(SYNTAQLITE_INLINE_DIALECT_DISPATCH)
#define SYNQ_PARSER_ALLOC(d, m, c)   SynqPerfettoParseAlloc(m, c)
#define SYNQ_PARSER_INIT(d, p, c)    SynqPerfettoParseInit(p, c)
#define SYNQ_PARSER_FINALIZE(d, p)   SynqPerfettoParseFinalize(p)
#define SYNQ_PARSER_FREE(d, p, f)    SynqPerfettoParseFree(p, f)
#define SYNQ_PARSER_FEED(d, p, t, m) SynqPerfettoParse(p, t, m)
#ifndef NDEBUG
#define SYNQ_PARSER_TRACE(d, f, s)   SynqPerfettoParseTrace(f, s)
#else
#define SYNQ_PARSER_TRACE(d, f, s)   ((void)0)
#endif
#define SYNQ_GET_TOKEN(env, z, t)    SynqPerfettoGetToken(env, z, t)
#endif

/* ======== begin: syntaqlite_dialect/dialect_types.h ======== */
#ifndef SYNTAQLITE_DIALECT_TYPES_H
#define SYNTAQLITE_DIALECT_TYPES_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Dialect-implementation types: full definitions of structs that appear in
// SyntaqliteDialectTemplate by pointer and are only needed when building a
// dialect descriptor. Consumer code (code that merely *uses* a dialect) needs
// only the forward declarations in syntaqlite/dialect.h.


#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Range metadata ───────────────────────────────────────────────────────────

typedef struct SyntaqliteFieldRangeMeta {
  uint16_t offset;
  uint8_t kind;
} SyntaqliteFieldRangeMeta;

typedef struct SyntaqliteRangeMetaEntry {
  const SyntaqliteFieldRangeMeta* fields;
  uint8_t count;
} SyntaqliteRangeMetaEntry;

// ── Field metadata (for AST dump / dynamic dialect loading) ──────────────────

#define SYNTAQLITE_FIELD_NODE_ID 0
#define SYNTAQLITE_FIELD_SPAN 1
#define SYNTAQLITE_FIELD_BOOL 2
#define SYNTAQLITE_FIELD_FLAGS 3
#define SYNTAQLITE_FIELD_ENUM 4

typedef struct SyntaqliteFieldMeta {
  uint16_t offset;             // byte offset in node struct
  uint8_t kind;                // SYNTAQLITE_FIELD_*
  const char* name;            // field name for AST dump
  const char* const* display;  // enum: indexed by ordinal; flags: indexed by
                               // bit pos; else NULL
  uint8_t display_count;       // number of entries in display[]
} SyntaqliteFieldMeta;

#ifdef __cplusplus
}
#endif


#endif  /* SYNTAQLITE_DIALECT_TYPES_H */
/* ======== end: syntaqlite_dialect/dialect_types.h ======== */

/* ======== begin: csrc/dialect_meta.h ======== */
#ifndef SYNTAQLITE_DIALECT_META_H
#define SYNTAQLITE_DIALECT_META_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT


#include <stddef.h>

static const char* const display_bool[] = {
    "FALSE",
    "TRUE",
};

static const char* const display_literal_type[] = {
    "INTEGER",
    "FLOAT",
    "STRING",
    "BLOB",
    "NULL",
    "CURRENT",
    "QNUMBER",
};

static const char* const display_binary_op[] = {
    "PLUS",
    "MINUS",
    "STAR",
    "SLASH",
    "REM",
    "LT",
    "GT",
    "LE",
    "GE",
    "EQ",
    "NE",
    "AND",
    "OR",
    "BIT_AND",
    "BIT_OR",
    "LSHIFT",
    "RSHIFT",
    "CONCAT",
    "PTR",
    "PTR2",
    "NE_ANGLE",
    "EQ_DOUBLE",
};

static const char* const display_unary_op[] = {
    "MINUS",
    "PLUS",
    "BIT_NOT",
    "NOT",
};

static const char* const display_compound_op[] = {
    "UNION",
    "UNION_ALL",
    "INTERSECT",
    "EXCEPT",
};

static const char* const display_is_op[] = {
    "IS",
    "IS_NOT",
    "IS_NULL",
    "NOT_NULL",
    "IS_NOT_DISTINCT",
    "IS_DISTINCT",
    "NOT_NULL_SPACED",
};

static const char* const display_like_keyword[] = {
    "LIKE",
    "GLOB",
    "MATCH",
    "REGEXP",
};

static const char* const display_temporary_qualifier[] = {
    "NONE",
    "TEMP",
    "TEMPORARY",
};

static const char* const display_foreign_key_action[] = {
    "UNSET",
    "NO_ACTION",
    "SET_NULL",
    "SET_DEFAULT",
    "CASCADE",
    "RESTRICT",
};

static const char* const display_deferrable[] = {
    "UNSET",
    "NOT_DEFERRABLE",
    "DEFERRABLE",
};

static const char* const display_initial_defer_mode[] = {
    "UNSET",
    "DEFERRED",
    "IMMEDIATE",
};

static const char* const display_generated_column_storage[] = {
    "NONE",
    "VIRTUAL",
    "STORED",
};

static const char* const display_column_constraint_type[] = {
    "DEFAULT",
    "NOT_NULL",
    "PRIMARY_KEY",
    "UNIQUE",
    "CHECK",
    "REFERENCES",
    "COLLATE",
    "GENERATED",
    "NULL",
    "DEFERRABLE",
};

static const char* const display_table_constraint_type[] = {
    "PRIMARY_KEY",
    "UNIQUE",
    "CHECK",
    "FOREIGN_KEY",
};

static const char* const display_foreign_key_option_kind[] = {
    "MATCH",
    "ON_DELETE",
    "ON_UPDATE",
    "ON_INSERT",
};

static const char* const display_materialized[] = {
    "DEFAULT",
    "MATERIALIZED",
    "NOT_MATERIALIZED",
};

static const char* const display_conflict_action[] = {
    "DEFAULT",
    "ROLLBACK",
    "ABORT",
    "FAIL",
    "IGNORE",
    "REPLACE",
};

static const char* const display_upsert_action[] = {
    "NOTHING",
    "UPDATE",
};

static const char* const display_index_hint[] = {
    "DEFAULT",
    "NOT_INDEXED",
    "INDEXED",
};

static const char* const display_insert_keyword[] = {
    "INSERT",
    "REPLACE",
};

static const char* const display_raise_type[] = {
    "IGNORE",
    "ROLLBACK",
    "ABORT",
    "FAIL",
};

static const char* const display_drop_object_type[] = {
    "TABLE",
    "INDEX",
    "VIEW",
    "TRIGGER",
};

static const char* const display_alter_op[] = {
    "RENAME_TABLE",
    "RENAME_COLUMN",
    "DROP_COLUMN",
    "ADD_COLUMN",
};

static const char* const display_transaction_type[] = {
    "NONE",
    "DEFERRED",
    "IMMEDIATE",
    "EXCLUSIVE",
};

static const char* const display_transaction_op[] = {
    "BEGIN",
    "COMMIT",
    "ROLLBACK",
    "END",
};

static const char* const display_savepoint_op[] = {
    "SAVEPOINT",
    "RELEASE",
    "ROLLBACK_TO",
};

static const char* const display_sort_order[] = {
    "NONE",
    "ASC",
    "DESC",
};

static const char* const display_nulls_order[] = {
    "NONE",
    "FIRST",
    "LAST",
};

static const char* const display_join_type[] = {
    "COMMA",
    "INNER",
    "LEFT",
    "RIGHT",
    "FULL",
    "CROSS",
    "NATURAL_INNER",
    "NATURAL_LEFT",
    "NATURAL_RIGHT",
    "NATURAL_FULL",
    "NATURAL_CROSS",
};

static const char* const display_join_modifier_kind[] = {
    "NATURAL",
    "LEFT",
    "OUTER",
    "RIGHT",
    "FULL",
    "INNER",
    "CROSS",
};

static const char* const display_trigger_timing[] = {
    "NONE",
    "BEFORE",
    "AFTER",
    "INSTEAD_OF",
};

static const char* const display_trigger_event_type[] = {
    "DELETE",
    "INSERT",
    "UPDATE",
};

static const char* const display_explain_mode[] = {
    "EXPLAIN",
    "QUERY_PLAN",
};

static const char* const display_pragma_form[] = {
    "BARE",
    "EQ",
    "CALL",
};

static const char* const display_analyze_or_reindex_op[] = {
    "ANALYZE",
    "REINDEX",
};

static const char* const display_frame_type[] = {
    "NONE",
    "RANGE",
    "ROWS",
    "GROUPS",
};

static const char* const display_frame_bound_type[] = {
    "UNBOUNDED_PRECEDING",
    "EXPR_PRECEDING",
    "CURRENT_ROW",
    "EXPR_FOLLOWING",
    "UNBOUNDED_FOLLOWING",
};

static const char* const display_frame_exclude[] = {
    "NONE",
    "NO_OTHERS",
    "CURRENT_ROW",
    "GROUP",
    "TIES",
};

static const char* const display_perfetto_return_kind[] = {
    "SCALAR",
    "TABLE",
};

static const char* const display_perfetto_tree_direction[] = {
    "UP",
    "DOWN",
};

static const char* const display_aggregate_function_call_flags[] = {
    "DISTINCT",
    "",
    "ALL",
};

static const char* const display_create_table_stmt_flags[] = {
    "WITHOUT_ROWID",
    "STRICT",
};

static const char* const display_function_call_flags[] = {
    "DISTINCT",
    "STAR",
    "ALL",
};

static const char* const display_result_column_flags[] = {
    "STAR",
};

static const char* const display_select_stmt_flags[] = {
    "DISTINCT",
    "",
    "ALL",
};

static const SyntaqliteFieldMeta field_meta_aggregate_function_call[] = {
    {offsetof(SyntaqliteAggregateFunctionCall, func_name), SYNTAQLITE_FIELD_SPAN, "func_name", NULL, 0},
    {offsetof(SyntaqliteAggregateFunctionCall, flags), SYNTAQLITE_FIELD_FLAGS, "flags", display_aggregate_function_call_flags, sizeof(display_aggregate_function_call_flags) / sizeof(display_aggregate_function_call_flags[0])},
    {offsetof(SyntaqliteAggregateFunctionCall, args), SYNTAQLITE_FIELD_NODE_ID, "args", NULL, 0},
    {offsetof(SyntaqliteAggregateFunctionCall, orderby), SYNTAQLITE_FIELD_NODE_ID, "orderby", NULL, 0},
    {offsetof(SyntaqliteAggregateFunctionCall, filter_clause), SYNTAQLITE_FIELD_NODE_ID, "filter_clause", NULL, 0},
    {offsetof(SyntaqliteAggregateFunctionCall, over_clause), SYNTAQLITE_FIELD_NODE_ID, "over_clause", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_ordered_set_function_call[] = {
    {offsetof(SyntaqliteOrderedSetFunctionCall, func_name), SYNTAQLITE_FIELD_SPAN, "func_name", NULL, 0},
    {offsetof(SyntaqliteOrderedSetFunctionCall, flags), SYNTAQLITE_FIELD_FLAGS, "flags", display_aggregate_function_call_flags, sizeof(display_aggregate_function_call_flags) / sizeof(display_aggregate_function_call_flags[0])},
    {offsetof(SyntaqliteOrderedSetFunctionCall, args), SYNTAQLITE_FIELD_NODE_ID, "args", NULL, 0},
    {offsetof(SyntaqliteOrderedSetFunctionCall, orderby_expr), SYNTAQLITE_FIELD_NODE_ID, "orderby_expr", NULL, 0},
    {offsetof(SyntaqliteOrderedSetFunctionCall, filter_clause), SYNTAQLITE_FIELD_NODE_ID, "filter_clause", NULL, 0},
    {offsetof(SyntaqliteOrderedSetFunctionCall, over_clause), SYNTAQLITE_FIELD_NODE_ID, "over_clause", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_cast_expr[] = {
    {offsetof(SyntaqliteCastExpr, expr), SYNTAQLITE_FIELD_NODE_ID, "expr", NULL, 0},
    {offsetof(SyntaqliteCastExpr, type_name), SYNTAQLITE_FIELD_SPAN, "type_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_column_ref[] = {
    {offsetof(SyntaqliteColumnRef, column), SYNTAQLITE_FIELD_SPAN, "column", NULL, 0},
    {offsetof(SyntaqliteColumnRef, table), SYNTAQLITE_FIELD_SPAN, "table", NULL, 0},
    {offsetof(SyntaqliteColumnRef, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_compound_select[] = {
    {offsetof(SyntaqliteCompoundSelect, op), SYNTAQLITE_FIELD_ENUM, "op", display_compound_op, sizeof(display_compound_op) / sizeof(display_compound_op[0])},
    {offsetof(SyntaqliteCompoundSelect, left), SYNTAQLITE_FIELD_NODE_ID, "left", NULL, 0},
    {offsetof(SyntaqliteCompoundSelect, right), SYNTAQLITE_FIELD_NODE_ID, "right", NULL, 0},
    {offsetof(SyntaqliteCompoundSelect, orderby), SYNTAQLITE_FIELD_NODE_ID, "orderby", NULL, 0},
    {offsetof(SyntaqliteCompoundSelect, limit_clause), SYNTAQLITE_FIELD_NODE_ID, "limit_clause", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_subquery_expr[] = {
    {offsetof(SyntaqliteSubqueryExpr, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_exists_expr[] = {
    {offsetof(SyntaqliteExistsExpr, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_in_expr[] = {
    {offsetof(SyntaqliteInExpr, negated), SYNTAQLITE_FIELD_BOOL, "negated", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteInExpr, bare_source), SYNTAQLITE_FIELD_BOOL, "bare_source", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteInExpr, operand), SYNTAQLITE_FIELD_NODE_ID, "operand", NULL, 0},
    {offsetof(SyntaqliteInExpr, source), SYNTAQLITE_FIELD_NODE_ID, "source", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_is_expr[] = {
    {offsetof(SyntaqliteIsExpr, op), SYNTAQLITE_FIELD_ENUM, "op", display_is_op, sizeof(display_is_op) / sizeof(display_is_op[0])},
    {offsetof(SyntaqliteIsExpr, left), SYNTAQLITE_FIELD_NODE_ID, "left", NULL, 0},
    {offsetof(SyntaqliteIsExpr, right), SYNTAQLITE_FIELD_NODE_ID, "right", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_between_expr[] = {
    {offsetof(SyntaqliteBetweenExpr, negated), SYNTAQLITE_FIELD_BOOL, "negated", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteBetweenExpr, operand), SYNTAQLITE_FIELD_NODE_ID, "operand", NULL, 0},
    {offsetof(SyntaqliteBetweenExpr, low), SYNTAQLITE_FIELD_NODE_ID, "low", NULL, 0},
    {offsetof(SyntaqliteBetweenExpr, high), SYNTAQLITE_FIELD_NODE_ID, "high", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_like_expr[] = {
    {offsetof(SyntaqliteLikeExpr, negated), SYNTAQLITE_FIELD_BOOL, "negated", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteLikeExpr, keyword), SYNTAQLITE_FIELD_ENUM, "keyword", display_like_keyword, sizeof(display_like_keyword) / sizeof(display_like_keyword[0])},
    {offsetof(SyntaqliteLikeExpr, operand), SYNTAQLITE_FIELD_NODE_ID, "operand", NULL, 0},
    {offsetof(SyntaqliteLikeExpr, pattern), SYNTAQLITE_FIELD_NODE_ID, "pattern", NULL, 0},
    {offsetof(SyntaqliteLikeExpr, escape), SYNTAQLITE_FIELD_NODE_ID, "escape", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_case_expr[] = {
    {offsetof(SyntaqliteCaseExpr, operand), SYNTAQLITE_FIELD_NODE_ID, "operand", NULL, 0},
    {offsetof(SyntaqliteCaseExpr, else_expr), SYNTAQLITE_FIELD_NODE_ID, "else_expr", NULL, 0},
    {offsetof(SyntaqliteCaseExpr, whens), SYNTAQLITE_FIELD_NODE_ID, "whens", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_case_when[] = {
    {offsetof(SyntaqliteCaseWhen, when_expr), SYNTAQLITE_FIELD_NODE_ID, "when_expr", NULL, 0},
    {offsetof(SyntaqliteCaseWhen, then_expr), SYNTAQLITE_FIELD_NODE_ID, "then_expr", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_foreign_key_option[] = {
    {offsetof(SyntaqliteForeignKeyOption, kind), SYNTAQLITE_FIELD_ENUM, "kind", display_foreign_key_option_kind, sizeof(display_foreign_key_option_kind) / sizeof(display_foreign_key_option_kind[0])},
    {offsetof(SyntaqliteForeignKeyOption, action), SYNTAQLITE_FIELD_ENUM, "action", display_foreign_key_action, sizeof(display_foreign_key_action) / sizeof(display_foreign_key_action[0])},
    {offsetof(SyntaqliteForeignKeyOption, match_name), SYNTAQLITE_FIELD_SPAN, "match_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_foreign_key_clause[] = {
    {offsetof(SyntaqliteForeignKeyClause, ref_table), SYNTAQLITE_FIELD_SPAN, "ref_table", NULL, 0},
    {offsetof(SyntaqliteForeignKeyClause, ref_columns), SYNTAQLITE_FIELD_NODE_ID, "ref_columns", NULL, 0},
    {offsetof(SyntaqliteForeignKeyClause, options), SYNTAQLITE_FIELD_NODE_ID, "options", NULL, 0},
    {offsetof(SyntaqliteForeignKeyClause, deferrable), SYNTAQLITE_FIELD_ENUM, "deferrable", display_deferrable, sizeof(display_deferrable) / sizeof(display_deferrable[0])},
    {offsetof(SyntaqliteForeignKeyClause, initial_defer), SYNTAQLITE_FIELD_ENUM, "initial_defer", display_initial_defer_mode, sizeof(display_initial_defer_mode) / sizeof(display_initial_defer_mode[0])},
};

static const SyntaqliteFieldMeta field_meta_column_constraint[] = {
    {offsetof(SyntaqliteColumnConstraint, kind), SYNTAQLITE_FIELD_ENUM, "kind", display_column_constraint_type, sizeof(display_column_constraint_type) / sizeof(display_column_constraint_type[0])},
    {offsetof(SyntaqliteColumnConstraint, onconf), SYNTAQLITE_FIELD_ENUM, "onconf", display_conflict_action, sizeof(display_conflict_action) / sizeof(display_conflict_action[0])},
    {offsetof(SyntaqliteColumnConstraint, sort_order), SYNTAQLITE_FIELD_ENUM, "sort_order", display_sort_order, sizeof(display_sort_order) / sizeof(display_sort_order[0])},
    {offsetof(SyntaqliteColumnConstraint, is_autoincrement), SYNTAQLITE_FIELD_BOOL, "is_autoincrement", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteColumnConstraint, collation_name), SYNTAQLITE_FIELD_SPAN, "collation_name", NULL, 0},
    {offsetof(SyntaqliteColumnConstraint, generated_storage), SYNTAQLITE_FIELD_ENUM, "generated_storage", display_generated_column_storage, sizeof(display_generated_column_storage) / sizeof(display_generated_column_storage[0])},
    {offsetof(SyntaqliteColumnConstraint, deferrable), SYNTAQLITE_FIELD_ENUM, "deferrable", display_deferrable, sizeof(display_deferrable) / sizeof(display_deferrable[0])},
    {offsetof(SyntaqliteColumnConstraint, initial_defer), SYNTAQLITE_FIELD_ENUM, "initial_defer", display_initial_defer_mode, sizeof(display_initial_defer_mode) / sizeof(display_initial_defer_mode[0])},
    {offsetof(SyntaqliteColumnConstraint, default_has_parens), SYNTAQLITE_FIELD_BOOL, "default_has_parens", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteColumnConstraint, generated_always), SYNTAQLITE_FIELD_BOOL, "generated_always", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteColumnConstraint, default_expr), SYNTAQLITE_FIELD_NODE_ID, "default_expr", NULL, 0},
    {offsetof(SyntaqliteColumnConstraint, check_expr), SYNTAQLITE_FIELD_NODE_ID, "check_expr", NULL, 0},
    {offsetof(SyntaqliteColumnConstraint, generated_expr), SYNTAQLITE_FIELD_NODE_ID, "generated_expr", NULL, 0},
    {offsetof(SyntaqliteColumnConstraint, fk_clause), SYNTAQLITE_FIELD_NODE_ID, "fk_clause", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_constraint_name_declaration[] = {
    {offsetof(SyntaqliteConstraintNameDeclaration, name), SYNTAQLITE_FIELD_SPAN, "name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_column_def[] = {
    {offsetof(SyntaqliteColumnDef, column_name), SYNTAQLITE_FIELD_NODE_ID, "column_name", NULL, 0},
    {offsetof(SyntaqliteColumnDef, type_name), SYNTAQLITE_FIELD_SPAN, "type_name", NULL, 0},
    {offsetof(SyntaqliteColumnDef, constraints), SYNTAQLITE_FIELD_NODE_ID, "constraints", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_table_constraint[] = {
    {offsetof(SyntaqliteTableConstraint, kind), SYNTAQLITE_FIELD_ENUM, "kind", display_table_constraint_type, sizeof(display_table_constraint_type) / sizeof(display_table_constraint_type[0])},
    {offsetof(SyntaqliteTableConstraint, onconf), SYNTAQLITE_FIELD_ENUM, "onconf", display_conflict_action, sizeof(display_conflict_action) / sizeof(display_conflict_action[0])},
    {offsetof(SyntaqliteTableConstraint, is_autoincrement), SYNTAQLITE_FIELD_BOOL, "is_autoincrement", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteTableConstraint, pk_columns), SYNTAQLITE_FIELD_NODE_ID, "pk_columns", NULL, 0},
    {offsetof(SyntaqliteTableConstraint, fk_columns), SYNTAQLITE_FIELD_NODE_ID, "fk_columns", NULL, 0},
    {offsetof(SyntaqliteTableConstraint, check_expr), SYNTAQLITE_FIELD_NODE_ID, "check_expr", NULL, 0},
    {offsetof(SyntaqliteTableConstraint, fk_clause), SYNTAQLITE_FIELD_NODE_ID, "fk_clause", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_table_stmt[] = {
    {offsetof(SyntaqliteCreateTableStmt, table_name), SYNTAQLITE_FIELD_SPAN, "table_name", NULL, 0},
    {offsetof(SyntaqliteCreateTableStmt, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqliteCreateTableStmt, temporary), SYNTAQLITE_FIELD_ENUM, "temporary", display_temporary_qualifier, sizeof(display_temporary_qualifier) / sizeof(display_temporary_qualifier[0])},
    {offsetof(SyntaqliteCreateTableStmt, if_not_exists), SYNTAQLITE_FIELD_BOOL, "if_not_exists", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreateTableStmt, flags), SYNTAQLITE_FIELD_FLAGS, "flags", display_create_table_stmt_flags, sizeof(display_create_table_stmt_flags) / sizeof(display_create_table_stmt_flags[0])},
    {offsetof(SyntaqliteCreateTableStmt, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
    {offsetof(SyntaqliteCreateTableStmt, table_constraints), SYNTAQLITE_FIELD_NODE_ID, "table_constraints", NULL, 0},
    {offsetof(SyntaqliteCreateTableStmt, as_select), SYNTAQLITE_FIELD_NODE_ID, "as_select", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_cte_definition[] = {
    {offsetof(SyntaqliteCteDefinition, cte_name), SYNTAQLITE_FIELD_SPAN, "cte_name", NULL, 0},
    {offsetof(SyntaqliteCteDefinition, materialized), SYNTAQLITE_FIELD_ENUM, "materialized", display_materialized, sizeof(display_materialized) / sizeof(display_materialized[0])},
    {offsetof(SyntaqliteCteDefinition, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
    {offsetof(SyntaqliteCteDefinition, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_with_clause[] = {
    {offsetof(SyntaqliteWithClause, recursive), SYNTAQLITE_FIELD_BOOL, "recursive", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteWithClause, ctes), SYNTAQLITE_FIELD_NODE_ID, "ctes", NULL, 0},
    {offsetof(SyntaqliteWithClause, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_upsert_clause[] = {
    {offsetof(SyntaqliteUpsertClause, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
    {offsetof(SyntaqliteUpsertClause, target_where), SYNTAQLITE_FIELD_NODE_ID, "target_where", NULL, 0},
    {offsetof(SyntaqliteUpsertClause, action), SYNTAQLITE_FIELD_ENUM, "action", display_upsert_action, sizeof(display_upsert_action) / sizeof(display_upsert_action[0])},
    {offsetof(SyntaqliteUpsertClause, setlist), SYNTAQLITE_FIELD_NODE_ID, "setlist", NULL, 0},
    {offsetof(SyntaqliteUpsertClause, update_where), SYNTAQLITE_FIELD_NODE_ID, "update_where", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_delete_stmt[] = {
    {offsetof(SyntaqliteDeleteStmt, with_ctes), SYNTAQLITE_FIELD_NODE_ID, "with_ctes", NULL, 0},
    {offsetof(SyntaqliteDeleteStmt, with_recursive), SYNTAQLITE_FIELD_BOOL, "with_recursive", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteDeleteStmt, table), SYNTAQLITE_FIELD_NODE_ID, "table", NULL, 0},
    {offsetof(SyntaqliteDeleteStmt, index_hint), SYNTAQLITE_FIELD_ENUM, "index_hint", display_index_hint, sizeof(display_index_hint) / sizeof(display_index_hint[0])},
    {offsetof(SyntaqliteDeleteStmt, index_name), SYNTAQLITE_FIELD_SPAN, "index_name", NULL, 0},
    {offsetof(SyntaqliteDeleteStmt, where_clause), SYNTAQLITE_FIELD_NODE_ID, "where_clause", NULL, 0},
    {offsetof(SyntaqliteDeleteStmt, orderby), SYNTAQLITE_FIELD_NODE_ID, "orderby", NULL, 0},
    {offsetof(SyntaqliteDeleteStmt, limit_clause), SYNTAQLITE_FIELD_NODE_ID, "limit_clause", NULL, 0},
    {offsetof(SyntaqliteDeleteStmt, returning), SYNTAQLITE_FIELD_NODE_ID, "returning", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_set_clause[] = {
    {offsetof(SyntaqliteSetClause, column), SYNTAQLITE_FIELD_SPAN, "column", NULL, 0},
    {offsetof(SyntaqliteSetClause, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
    {offsetof(SyntaqliteSetClause, value), SYNTAQLITE_FIELD_NODE_ID, "value", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_update_stmt[] = {
    {offsetof(SyntaqliteUpdateStmt, with_ctes), SYNTAQLITE_FIELD_NODE_ID, "with_ctes", NULL, 0},
    {offsetof(SyntaqliteUpdateStmt, with_recursive), SYNTAQLITE_FIELD_BOOL, "with_recursive", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteUpdateStmt, conflict_action), SYNTAQLITE_FIELD_ENUM, "conflict_action", display_conflict_action, sizeof(display_conflict_action) / sizeof(display_conflict_action[0])},
    {offsetof(SyntaqliteUpdateStmt, table), SYNTAQLITE_FIELD_NODE_ID, "table", NULL, 0},
    {offsetof(SyntaqliteUpdateStmt, index_hint), SYNTAQLITE_FIELD_ENUM, "index_hint", display_index_hint, sizeof(display_index_hint) / sizeof(display_index_hint[0])},
    {offsetof(SyntaqliteUpdateStmt, index_name), SYNTAQLITE_FIELD_SPAN, "index_name", NULL, 0},
    {offsetof(SyntaqliteUpdateStmt, setlist), SYNTAQLITE_FIELD_NODE_ID, "setlist", NULL, 0},
    {offsetof(SyntaqliteUpdateStmt, from_clause), SYNTAQLITE_FIELD_NODE_ID, "from_clause", NULL, 0},
    {offsetof(SyntaqliteUpdateStmt, where_clause), SYNTAQLITE_FIELD_NODE_ID, "where_clause", NULL, 0},
    {offsetof(SyntaqliteUpdateStmt, orderby), SYNTAQLITE_FIELD_NODE_ID, "orderby", NULL, 0},
    {offsetof(SyntaqliteUpdateStmt, limit_clause), SYNTAQLITE_FIELD_NODE_ID, "limit_clause", NULL, 0},
    {offsetof(SyntaqliteUpdateStmt, returning), SYNTAQLITE_FIELD_NODE_ID, "returning", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_insert_stmt[] = {
    {offsetof(SyntaqliteInsertStmt, with_ctes), SYNTAQLITE_FIELD_NODE_ID, "with_ctes", NULL, 0},
    {offsetof(SyntaqliteInsertStmt, with_recursive), SYNTAQLITE_FIELD_BOOL, "with_recursive", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteInsertStmt, keyword), SYNTAQLITE_FIELD_ENUM, "keyword", display_insert_keyword, sizeof(display_insert_keyword) / sizeof(display_insert_keyword[0])},
    {offsetof(SyntaqliteInsertStmt, conflict_action), SYNTAQLITE_FIELD_ENUM, "conflict_action", display_conflict_action, sizeof(display_conflict_action) / sizeof(display_conflict_action[0])},
    {offsetof(SyntaqliteInsertStmt, table), SYNTAQLITE_FIELD_NODE_ID, "table", NULL, 0},
    {offsetof(SyntaqliteInsertStmt, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
    {offsetof(SyntaqliteInsertStmt, source), SYNTAQLITE_FIELD_NODE_ID, "source", NULL, 0},
    {offsetof(SyntaqliteInsertStmt, upsert), SYNTAQLITE_FIELD_NODE_ID, "upsert", NULL, 0},
    {offsetof(SyntaqliteInsertStmt, returning), SYNTAQLITE_FIELD_NODE_ID, "returning", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_binary_expr[] = {
    {offsetof(SyntaqliteBinaryExpr, op), SYNTAQLITE_FIELD_ENUM, "op", display_binary_op, sizeof(display_binary_op) / sizeof(display_binary_op[0])},
    {offsetof(SyntaqliteBinaryExpr, left), SYNTAQLITE_FIELD_NODE_ID, "left", NULL, 0},
    {offsetof(SyntaqliteBinaryExpr, right), SYNTAQLITE_FIELD_NODE_ID, "right", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_unary_expr[] = {
    {offsetof(SyntaqliteUnaryExpr, op), SYNTAQLITE_FIELD_ENUM, "op", display_unary_op, sizeof(display_unary_op) / sizeof(display_unary_op[0])},
    {offsetof(SyntaqliteUnaryExpr, operand), SYNTAQLITE_FIELD_NODE_ID, "operand", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_literal[] = {
    {offsetof(SyntaqliteLiteral, literal_type), SYNTAQLITE_FIELD_ENUM, "literal_type", display_literal_type, sizeof(display_literal_type) / sizeof(display_literal_type[0])},
    {offsetof(SyntaqliteLiteral, source), SYNTAQLITE_FIELD_SPAN, "source", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_paren_expr[] = {
    {offsetof(SyntaqliteParenExpr, expr), SYNTAQLITE_FIELD_NODE_ID, "expr", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_ident_name[] = {
    {offsetof(SyntaqliteIdentName, source), SYNTAQLITE_FIELD_SPAN, "source", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_error[] = {
    {offsetof(SyntaqliteError, source), SYNTAQLITE_FIELD_SPAN, "source", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_row_value[] = {
    {offsetof(SyntaqliteRowValue, items), SYNTAQLITE_FIELD_NODE_ID, "items", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_function_call[] = {
    {offsetof(SyntaqliteFunctionCall, func_name), SYNTAQLITE_FIELD_SPAN, "func_name", NULL, 0},
    {offsetof(SyntaqliteFunctionCall, flags), SYNTAQLITE_FIELD_FLAGS, "flags", display_function_call_flags, sizeof(display_function_call_flags) / sizeof(display_function_call_flags[0])},
    {offsetof(SyntaqliteFunctionCall, args), SYNTAQLITE_FIELD_NODE_ID, "args", NULL, 0},
    {offsetof(SyntaqliteFunctionCall, filter_clause), SYNTAQLITE_FIELD_NODE_ID, "filter_clause", NULL, 0},
    {offsetof(SyntaqliteFunctionCall, over_clause), SYNTAQLITE_FIELD_NODE_ID, "over_clause", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_variable[] = {
    {offsetof(SyntaqliteVariable, source), SYNTAQLITE_FIELD_SPAN, "source", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_collate_expr[] = {
    {offsetof(SyntaqliteCollateExpr, expr), SYNTAQLITE_FIELD_NODE_ID, "expr", NULL, 0},
    {offsetof(SyntaqliteCollateExpr, collation), SYNTAQLITE_FIELD_SPAN, "collation", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_raise_expr[] = {
    {offsetof(SyntaqliteRaiseExpr, raise_type), SYNTAQLITE_FIELD_ENUM, "raise_type", display_raise_type, sizeof(display_raise_type) / sizeof(display_raise_type[0])},
    {offsetof(SyntaqliteRaiseExpr, error_message), SYNTAQLITE_FIELD_NODE_ID, "error_message", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_qualified_name[] = {
    {offsetof(SyntaqliteQualifiedName, object_name), SYNTAQLITE_FIELD_NODE_ID, "object_name", NULL, 0},
    {offsetof(SyntaqliteQualifiedName, schema), SYNTAQLITE_FIELD_NODE_ID, "schema", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_drop_stmt[] = {
    {offsetof(SyntaqliteDropStmt, object_type), SYNTAQLITE_FIELD_ENUM, "object_type", display_drop_object_type, sizeof(display_drop_object_type) / sizeof(display_drop_object_type[0])},
    {offsetof(SyntaqliteDropStmt, if_exists), SYNTAQLITE_FIELD_BOOL, "if_exists", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteDropStmt, target), SYNTAQLITE_FIELD_NODE_ID, "target", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_alter_table_stmt[] = {
    {offsetof(SyntaqliteAlterTableStmt, op), SYNTAQLITE_FIELD_ENUM, "op", display_alter_op, sizeof(display_alter_op) / sizeof(display_alter_op[0])},
    {offsetof(SyntaqliteAlterTableStmt, has_column_kw), SYNTAQLITE_FIELD_BOOL, "has_column_kw", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteAlterTableStmt, target), SYNTAQLITE_FIELD_NODE_ID, "target", NULL, 0},
    {offsetof(SyntaqliteAlterTableStmt, new_name), SYNTAQLITE_FIELD_NODE_ID, "new_name", NULL, 0},
    {offsetof(SyntaqliteAlterTableStmt, old_name), SYNTAQLITE_FIELD_NODE_ID, "old_name", NULL, 0},
    {offsetof(SyntaqliteAlterTableStmt, column), SYNTAQLITE_FIELD_NODE_ID, "column", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_transaction_stmt[] = {
    {offsetof(SyntaqliteTransactionStmt, op), SYNTAQLITE_FIELD_ENUM, "op", display_transaction_op, sizeof(display_transaction_op) / sizeof(display_transaction_op[0])},
    {offsetof(SyntaqliteTransactionStmt, trans_type), SYNTAQLITE_FIELD_ENUM, "trans_type", display_transaction_type, sizeof(display_transaction_type) / sizeof(display_transaction_type[0])},
    {offsetof(SyntaqliteTransactionStmt, has_transaction), SYNTAQLITE_FIELD_BOOL, "has_transaction", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteTransactionStmt, name), SYNTAQLITE_FIELD_SPAN, "name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_savepoint_stmt[] = {
    {offsetof(SyntaqliteSavepointStmt, op), SYNTAQLITE_FIELD_ENUM, "op", display_savepoint_op, sizeof(display_savepoint_op) / sizeof(display_savepoint_op[0])},
    {offsetof(SyntaqliteSavepointStmt, savepoint_name), SYNTAQLITE_FIELD_NODE_ID, "savepoint_name", NULL, 0},
    {offsetof(SyntaqliteSavepointStmt, has_savepoint), SYNTAQLITE_FIELD_BOOL, "has_savepoint", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteSavepointStmt, has_transaction), SYNTAQLITE_FIELD_BOOL, "has_transaction", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteSavepointStmt, transaction_name), SYNTAQLITE_FIELD_SPAN, "transaction_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_result_column[] = {
    {offsetof(SyntaqliteResultColumn, flags), SYNTAQLITE_FIELD_FLAGS, "flags", display_result_column_flags, sizeof(display_result_column_flags) / sizeof(display_result_column_flags[0])},
    {offsetof(SyntaqliteResultColumn, alias), SYNTAQLITE_FIELD_NODE_ID, "alias", NULL, 0},
    {offsetof(SyntaqliteResultColumn, alias_as), SYNTAQLITE_FIELD_BOOL, "alias_as", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteResultColumn, expr), SYNTAQLITE_FIELD_NODE_ID, "expr", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_select_stmt[] = {
    {offsetof(SyntaqliteSelectStmt, flags), SYNTAQLITE_FIELD_FLAGS, "flags", display_select_stmt_flags, sizeof(display_select_stmt_flags) / sizeof(display_select_stmt_flags[0])},
    {offsetof(SyntaqliteSelectStmt, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
    {offsetof(SyntaqliteSelectStmt, from_clause), SYNTAQLITE_FIELD_NODE_ID, "from_clause", NULL, 0},
    {offsetof(SyntaqliteSelectStmt, where_clause), SYNTAQLITE_FIELD_NODE_ID, "where_clause", NULL, 0},
    {offsetof(SyntaqliteSelectStmt, groupby), SYNTAQLITE_FIELD_NODE_ID, "groupby", NULL, 0},
    {offsetof(SyntaqliteSelectStmt, having), SYNTAQLITE_FIELD_NODE_ID, "having", NULL, 0},
    {offsetof(SyntaqliteSelectStmt, orderby), SYNTAQLITE_FIELD_NODE_ID, "orderby", NULL, 0},
    {offsetof(SyntaqliteSelectStmt, limit_clause), SYNTAQLITE_FIELD_NODE_ID, "limit_clause", NULL, 0},
    {offsetof(SyntaqliteSelectStmt, window_clause), SYNTAQLITE_FIELD_NODE_ID, "window_clause", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_ordering_term[] = {
    {offsetof(SyntaqliteOrderingTerm, expr), SYNTAQLITE_FIELD_NODE_ID, "expr", NULL, 0},
    {offsetof(SyntaqliteOrderingTerm, sort_order), SYNTAQLITE_FIELD_ENUM, "sort_order", display_sort_order, sizeof(display_sort_order) / sizeof(display_sort_order[0])},
    {offsetof(SyntaqliteOrderingTerm, nulls_order), SYNTAQLITE_FIELD_ENUM, "nulls_order", display_nulls_order, sizeof(display_nulls_order) / sizeof(display_nulls_order[0])},
};

static const SyntaqliteFieldMeta field_meta_limit_clause[] = {
    {offsetof(SyntaqliteLimitClause, limit), SYNTAQLITE_FIELD_NODE_ID, "limit", NULL, 0},
    {offsetof(SyntaqliteLimitClause, offset), SYNTAQLITE_FIELD_NODE_ID, "offset", NULL, 0},
    {offsetof(SyntaqliteLimitClause, comma_form), SYNTAQLITE_FIELD_BOOL, "comma_form", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
};

static const SyntaqliteFieldMeta field_meta_join_modifier[] = {
    {offsetof(SyntaqliteJoinModifier, kind), SYNTAQLITE_FIELD_ENUM, "kind", display_join_modifier_kind, sizeof(display_join_modifier_kind) / sizeof(display_join_modifier_kind[0])},
};

static const SyntaqliteFieldMeta field_meta_table_ref[] = {
    {offsetof(SyntaqliteTableRef, table_name), SYNTAQLITE_FIELD_SPAN, "table_name", NULL, 0},
    {offsetof(SyntaqliteTableRef, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqliteTableRef, has_parens), SYNTAQLITE_FIELD_BOOL, "has_parens", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteTableRef, alias), SYNTAQLITE_FIELD_NODE_ID, "alias", NULL, 0},
    {offsetof(SyntaqliteTableRef, alias_as), SYNTAQLITE_FIELD_BOOL, "alias_as", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteTableRef, args), SYNTAQLITE_FIELD_NODE_ID, "args", NULL, 0},
    {offsetof(SyntaqliteTableRef, index_hint), SYNTAQLITE_FIELD_ENUM, "index_hint", display_index_hint, sizeof(display_index_hint) / sizeof(display_index_hint[0])},
    {offsetof(SyntaqliteTableRef, index_name), SYNTAQLITE_FIELD_SPAN, "index_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_subquery_table_source[] = {
    {offsetof(SyntaqliteSubqueryTableSource, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
    {offsetof(SyntaqliteSubqueryTableSource, alias), SYNTAQLITE_FIELD_NODE_ID, "alias", NULL, 0},
    {offsetof(SyntaqliteSubqueryTableSource, alias_as), SYNTAQLITE_FIELD_BOOL, "alias_as", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
};

static const SyntaqliteFieldMeta field_meta_paren_table_source[] = {
    {offsetof(SyntaqliteParenTableSource, source), SYNTAQLITE_FIELD_NODE_ID, "source", NULL, 0},
    {offsetof(SyntaqliteParenTableSource, alias), SYNTAQLITE_FIELD_NODE_ID, "alias", NULL, 0},
    {offsetof(SyntaqliteParenTableSource, alias_as), SYNTAQLITE_FIELD_BOOL, "alias_as", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
};

static const SyntaqliteFieldMeta field_meta_join_clause[] = {
    {offsetof(SyntaqliteJoinClause, join_type), SYNTAQLITE_FIELD_ENUM, "join_type", display_join_type, sizeof(display_join_type) / sizeof(display_join_type[0])},
    {offsetof(SyntaqliteJoinClause, modifiers), SYNTAQLITE_FIELD_NODE_ID, "modifiers", NULL, 0},
    {offsetof(SyntaqliteJoinClause, left), SYNTAQLITE_FIELD_NODE_ID, "left", NULL, 0},
    {offsetof(SyntaqliteJoinClause, right), SYNTAQLITE_FIELD_NODE_ID, "right", NULL, 0},
    {offsetof(SyntaqliteJoinClause, on_expr), SYNTAQLITE_FIELD_NODE_ID, "on_expr", NULL, 0},
    {offsetof(SyntaqliteJoinClause, using_columns), SYNTAQLITE_FIELD_NODE_ID, "using_columns", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_join_prefix[] = {
    {offsetof(SyntaqliteJoinPrefix, source), SYNTAQLITE_FIELD_NODE_ID, "source", NULL, 0},
    {offsetof(SyntaqliteJoinPrefix, join_type), SYNTAQLITE_FIELD_ENUM, "join_type", display_join_type, sizeof(display_join_type) / sizeof(display_join_type[0])},
    {offsetof(SyntaqliteJoinPrefix, modifiers), SYNTAQLITE_FIELD_NODE_ID, "modifiers", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_trigger_event[] = {
    {offsetof(SyntaqliteTriggerEvent, event_type), SYNTAQLITE_FIELD_ENUM, "event_type", display_trigger_event_type, sizeof(display_trigger_event_type) / sizeof(display_trigger_event_type[0])},
    {offsetof(SyntaqliteTriggerEvent, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_trigger_stmt[] = {
    {offsetof(SyntaqliteCreateTriggerStmt, trigger_name), SYNTAQLITE_FIELD_SPAN, "trigger_name", NULL, 0},
    {offsetof(SyntaqliteCreateTriggerStmt, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqliteCreateTriggerStmt, temporary), SYNTAQLITE_FIELD_ENUM, "temporary", display_temporary_qualifier, sizeof(display_temporary_qualifier) / sizeof(display_temporary_qualifier[0])},
    {offsetof(SyntaqliteCreateTriggerStmt, if_not_exists), SYNTAQLITE_FIELD_BOOL, "if_not_exists", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreateTriggerStmt, timing), SYNTAQLITE_FIELD_ENUM, "timing", display_trigger_timing, sizeof(display_trigger_timing) / sizeof(display_trigger_timing[0])},
    {offsetof(SyntaqliteCreateTriggerStmt, for_each_row), SYNTAQLITE_FIELD_BOOL, "for_each_row", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreateTriggerStmt, event), SYNTAQLITE_FIELD_NODE_ID, "event", NULL, 0},
    {offsetof(SyntaqliteCreateTriggerStmt, table), SYNTAQLITE_FIELD_NODE_ID, "table", NULL, 0},
    {offsetof(SyntaqliteCreateTriggerStmt, when_expr), SYNTAQLITE_FIELD_NODE_ID, "when_expr", NULL, 0},
    {offsetof(SyntaqliteCreateTriggerStmt, body), SYNTAQLITE_FIELD_NODE_ID, "body", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_virtual_table_stmt[] = {
    {offsetof(SyntaqliteCreateVirtualTableStmt, table_name), SYNTAQLITE_FIELD_SPAN, "table_name", NULL, 0},
    {offsetof(SyntaqliteCreateVirtualTableStmt, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqliteCreateVirtualTableStmt, module_name), SYNTAQLITE_FIELD_SPAN, "module_name", NULL, 0},
    {offsetof(SyntaqliteCreateVirtualTableStmt, if_not_exists), SYNTAQLITE_FIELD_BOOL, "if_not_exists", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreateVirtualTableStmt, has_module_args), SYNTAQLITE_FIELD_BOOL, "has_module_args", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreateVirtualTableStmt, module_args), SYNTAQLITE_FIELD_SPAN, "module_args", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_pragma_stmt[] = {
    {offsetof(SyntaqlitePragmaStmt, pragma_name), SYNTAQLITE_FIELD_SPAN, "pragma_name", NULL, 0},
    {offsetof(SyntaqlitePragmaStmt, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqlitePragmaStmt, value), SYNTAQLITE_FIELD_SPAN, "value", NULL, 0},
    {offsetof(SyntaqlitePragmaStmt, pragma_form), SYNTAQLITE_FIELD_ENUM, "pragma_form", display_pragma_form, sizeof(display_pragma_form) / sizeof(display_pragma_form[0])},
};

static const SyntaqliteFieldMeta field_meta_analyze_or_reindex_stmt[] = {
    {offsetof(SyntaqliteAnalyzeOrReindexStmt, target_name), SYNTAQLITE_FIELD_SPAN, "target_name", NULL, 0},
    {offsetof(SyntaqliteAnalyzeOrReindexStmt, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqliteAnalyzeOrReindexStmt, kind), SYNTAQLITE_FIELD_ENUM, "kind", display_analyze_or_reindex_op, sizeof(display_analyze_or_reindex_op) / sizeof(display_analyze_or_reindex_op[0])},
};

static const SyntaqliteFieldMeta field_meta_attach_stmt[] = {
    {offsetof(SyntaqliteAttachStmt, has_database), SYNTAQLITE_FIELD_BOOL, "has_database", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteAttachStmt, filename), SYNTAQLITE_FIELD_NODE_ID, "filename", NULL, 0},
    {offsetof(SyntaqliteAttachStmt, db_name), SYNTAQLITE_FIELD_NODE_ID, "db_name", NULL, 0},
    {offsetof(SyntaqliteAttachStmt, key), SYNTAQLITE_FIELD_NODE_ID, "key", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_detach_stmt[] = {
    {offsetof(SyntaqliteDetachStmt, has_database), SYNTAQLITE_FIELD_BOOL, "has_database", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteDetachStmt, db_name), SYNTAQLITE_FIELD_NODE_ID, "db_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_vacuum_stmt[] = {
    {offsetof(SyntaqliteVacuumStmt, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqliteVacuumStmt, filename), SYNTAQLITE_FIELD_NODE_ID, "filename", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_explain_stmt[] = {
    {offsetof(SyntaqliteExplainStmt, explain_mode), SYNTAQLITE_FIELD_ENUM, "explain_mode", display_explain_mode, sizeof(display_explain_mode) / sizeof(display_explain_mode[0])},
    {offsetof(SyntaqliteExplainStmt, stmt), SYNTAQLITE_FIELD_NODE_ID, "stmt", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_index_stmt[] = {
    {offsetof(SyntaqliteCreateIndexStmt, index_name), SYNTAQLITE_FIELD_SPAN, "index_name", NULL, 0},
    {offsetof(SyntaqliteCreateIndexStmt, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqliteCreateIndexStmt, table_name), SYNTAQLITE_FIELD_SPAN, "table_name", NULL, 0},
    {offsetof(SyntaqliteCreateIndexStmt, is_unique), SYNTAQLITE_FIELD_BOOL, "is_unique", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreateIndexStmt, if_not_exists), SYNTAQLITE_FIELD_BOOL, "if_not_exists", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreateIndexStmt, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
    {offsetof(SyntaqliteCreateIndexStmt, where_clause), SYNTAQLITE_FIELD_NODE_ID, "where_clause", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_view_stmt[] = {
    {offsetof(SyntaqliteCreateViewStmt, view_name), SYNTAQLITE_FIELD_SPAN, "view_name", NULL, 0},
    {offsetof(SyntaqliteCreateViewStmt, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqliteCreateViewStmt, temporary), SYNTAQLITE_FIELD_ENUM, "temporary", display_temporary_qualifier, sizeof(display_temporary_qualifier) / sizeof(display_temporary_qualifier[0])},
    {offsetof(SyntaqliteCreateViewStmt, if_not_exists), SYNTAQLITE_FIELD_BOOL, "if_not_exists", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreateViewStmt, column_names), SYNTAQLITE_FIELD_NODE_ID, "column_names", NULL, 0},
    {offsetof(SyntaqliteCreateViewStmt, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_values_clause[] = {
    {offsetof(SyntaqliteValuesClause, rows), SYNTAQLITE_FIELD_NODE_ID, "rows", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_frame_bound[] = {
    {offsetof(SyntaqliteFrameBound, bound_type), SYNTAQLITE_FIELD_ENUM, "bound_type", display_frame_bound_type, sizeof(display_frame_bound_type) / sizeof(display_frame_bound_type[0])},
    {offsetof(SyntaqliteFrameBound, expr), SYNTAQLITE_FIELD_NODE_ID, "expr", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_frame_spec[] = {
    {offsetof(SyntaqliteFrameSpec, frame_type), SYNTAQLITE_FIELD_ENUM, "frame_type", display_frame_type, sizeof(display_frame_type) / sizeof(display_frame_type[0])},
    {offsetof(SyntaqliteFrameSpec, exclude), SYNTAQLITE_FIELD_ENUM, "exclude", display_frame_exclude, sizeof(display_frame_exclude) / sizeof(display_frame_exclude[0])},
    {offsetof(SyntaqliteFrameSpec, start_bound), SYNTAQLITE_FIELD_NODE_ID, "start_bound", NULL, 0},
    {offsetof(SyntaqliteFrameSpec, end_bound), SYNTAQLITE_FIELD_NODE_ID, "end_bound", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_window_def[] = {
    {offsetof(SyntaqliteWindowDef, ref_window_name), SYNTAQLITE_FIELD_SPAN, "ref_window_name", NULL, 0},
    {offsetof(SyntaqliteWindowDef, base_window_name), SYNTAQLITE_FIELD_SPAN, "base_window_name", NULL, 0},
    {offsetof(SyntaqliteWindowDef, partition_by), SYNTAQLITE_FIELD_NODE_ID, "partition_by", NULL, 0},
    {offsetof(SyntaqliteWindowDef, orderby), SYNTAQLITE_FIELD_NODE_ID, "orderby", NULL, 0},
    {offsetof(SyntaqliteWindowDef, frame), SYNTAQLITE_FIELD_NODE_ID, "frame", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_named_window_def[] = {
    {offsetof(SyntaqliteNamedWindowDef, window_name), SYNTAQLITE_FIELD_SPAN, "window_name", NULL, 0},
    {offsetof(SyntaqliteNamedWindowDef, window_def), SYNTAQLITE_FIELD_NODE_ID, "window_def", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_filter_over[] = {
    {offsetof(SyntaqliteFilterOver, filter_expr), SYNTAQLITE_FIELD_NODE_ID, "filter_expr", NULL, 0},
    {offsetof(SyntaqliteFilterOver, over_def), SYNTAQLITE_FIELD_NODE_ID, "over_def", NULL, 0},
    {offsetof(SyntaqliteFilterOver, over_name), SYNTAQLITE_FIELD_SPAN, "over_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_arg_def[] = {
    {offsetof(SyntaqlitePerfettoArgDef, arg_name), SYNTAQLITE_FIELD_NODE_ID, "arg_name", NULL, 0},
    {offsetof(SyntaqlitePerfettoArgDef, arg_type), SYNTAQLITE_FIELD_SPAN, "arg_type", NULL, 0},
    {offsetof(SyntaqlitePerfettoArgDef, is_variadic), SYNTAQLITE_FIELD_BOOL, "is_variadic", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
};

static const SyntaqliteFieldMeta field_meta_perfetto_macro_arg[] = {
    {offsetof(SyntaqlitePerfettoMacroArg, arg_name), SYNTAQLITE_FIELD_SPAN, "arg_name", NULL, 0},
    {offsetof(SyntaqlitePerfettoMacroArg, arg_type), SYNTAQLITE_FIELD_SPAN, "arg_type", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_indexed_column[] = {
    {offsetof(SyntaqlitePerfettoIndexedColumn, column_name), SYNTAQLITE_FIELD_SPAN, "column_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_return_type[] = {
    {offsetof(SyntaqlitePerfettoReturnType, kind), SYNTAQLITE_FIELD_ENUM, "kind", display_perfetto_return_kind, sizeof(display_perfetto_return_kind) / sizeof(display_perfetto_return_kind[0])},
    {offsetof(SyntaqlitePerfettoReturnType, scalar_type), SYNTAQLITE_FIELD_SPAN, "scalar_type", NULL, 0},
    {offsetof(SyntaqlitePerfettoReturnType, table_columns), SYNTAQLITE_FIELD_NODE_ID, "table_columns", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_table_impl[] = {
    {offsetof(SyntaqlitePerfettoTableImpl, name), SYNTAQLITE_FIELD_SPAN, "name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_perfetto_table_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoTableStmt, table_name), SYNTAQLITE_FIELD_SPAN, "table_name", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoTableStmt, or_replace), SYNTAQLITE_FIELD_BOOL, "or_replace", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreatePerfettoTableStmt, table_impl), SYNTAQLITE_FIELD_NODE_ID, "table_impl", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoTableStmt, schema), SYNTAQLITE_FIELD_NODE_ID, "schema", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoTableStmt, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoTableStmt, select_span), SYNTAQLITE_FIELD_SPAN, "select_span", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoTableStmt, pipeline), SYNTAQLITE_FIELD_NODE_ID, "pipeline", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_perfetto_view_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoViewStmt, view_name), SYNTAQLITE_FIELD_SPAN, "view_name", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoViewStmt, or_replace), SYNTAQLITE_FIELD_BOOL, "or_replace", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreatePerfettoViewStmt, schema), SYNTAQLITE_FIELD_NODE_ID, "schema", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoViewStmt, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoViewStmt, select_span), SYNTAQLITE_FIELD_SPAN, "select_span", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_perfetto_function_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoFunctionStmt, function_name), SYNTAQLITE_FIELD_SPAN, "function_name", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoFunctionStmt, or_replace), SYNTAQLITE_FIELD_BOOL, "or_replace", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreatePerfettoFunctionStmt, args), SYNTAQLITE_FIELD_NODE_ID, "args", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoFunctionStmt, return_type), SYNTAQLITE_FIELD_NODE_ID, "return_type", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoFunctionStmt, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoFunctionStmt, select_span), SYNTAQLITE_FIELD_SPAN, "select_span", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_perfetto_delegating_function_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoDelegatingFunctionStmt, function_name), SYNTAQLITE_FIELD_SPAN, "function_name", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoDelegatingFunctionStmt, or_replace), SYNTAQLITE_FIELD_BOOL, "or_replace", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreatePerfettoDelegatingFunctionStmt, args), SYNTAQLITE_FIELD_NODE_ID, "args", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoDelegatingFunctionStmt, return_type), SYNTAQLITE_FIELD_NODE_ID, "return_type", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoDelegatingFunctionStmt, delegate_to), SYNTAQLITE_FIELD_SPAN, "delegate_to", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_perfetto_index_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoIndexStmt, index_name), SYNTAQLITE_FIELD_SPAN, "index_name", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoIndexStmt, or_replace), SYNTAQLITE_FIELD_BOOL, "or_replace", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreatePerfettoIndexStmt, table_name), SYNTAQLITE_FIELD_SPAN, "table_name", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoIndexStmt, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_create_perfetto_macro_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoMacroStmt, macro_name), SYNTAQLITE_FIELD_SPAN, "macro_name", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoMacroStmt, or_replace), SYNTAQLITE_FIELD_BOOL, "or_replace", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
    {offsetof(SyntaqliteCreatePerfettoMacroStmt, return_type), SYNTAQLITE_FIELD_SPAN, "return_type", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoMacroStmt, body), SYNTAQLITE_FIELD_SPAN, "body", NULL, 0},
    {offsetof(SyntaqliteCreatePerfettoMacroStmt, args), SYNTAQLITE_FIELD_NODE_ID, "args", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_include_perfetto_module_stmt[] = {
    {offsetof(SyntaqliteIncludePerfettoModuleStmt, module_name), SYNTAQLITE_FIELD_SPAN, "module_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_drop_perfetto_index_stmt[] = {
    {offsetof(SyntaqliteDropPerfettoIndexStmt, index_name), SYNTAQLITE_FIELD_SPAN, "index_name", NULL, 0},
    {offsetof(SyntaqliteDropPerfettoIndexStmt, table_name), SYNTAQLITE_FIELD_SPAN, "table_name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_pragma_stmt[] = {
    {offsetof(SyntaqlitePerfettoPragmaStmt, name), SYNTAQLITE_FIELD_SPAN, "name", NULL, 0},
    {offsetof(SyntaqlitePerfettoPragmaStmt, value), SYNTAQLITE_FIELD_NODE_ID, "value", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_pipe_source[] = {
    {offsetof(SyntaqlitePerfettoPipeSource, table_name), SYNTAQLITE_FIELD_SPAN, "table_name", NULL, 0},
    {offsetof(SyntaqlitePerfettoPipeSource, schema), SYNTAQLITE_FIELD_SPAN, "schema", NULL, 0},
    {offsetof(SyntaqlitePerfettoPipeSource, select), SYNTAQLITE_FIELD_NODE_ID, "select", NULL, 0},
    {offsetof(SyntaqlitePerfettoPipeSource, alias), SYNTAQLITE_FIELD_NODE_ID, "alias", NULL, 0},
    {offsetof(SyntaqlitePerfettoPipeSource, alias_as), SYNTAQLITE_FIELD_BOOL, "alias_as", display_bool, sizeof(display_bool) / sizeof(display_bool[0])},
};

static const SyntaqliteFieldMeta field_meta_perfetto_tree_aggregate[] = {
    {offsetof(SyntaqlitePerfettoTreeAggregate, expr), SYNTAQLITE_FIELD_NODE_ID, "expr", NULL, 0},
    {offsetof(SyntaqlitePerfettoTreeAggregate, name), SYNTAQLITE_FIELD_SPAN, "name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_tree_accumulate[] = {
    {offsetof(SyntaqlitePerfettoTreeAccumulate, direction), SYNTAQLITE_FIELD_ENUM, "direction", display_perfetto_tree_direction, sizeof(display_perfetto_tree_direction) / sizeof(display_perfetto_tree_direction[0])},
    {offsetof(SyntaqlitePerfettoTreeAccumulate, aggregates), SYNTAQLITE_FIELD_NODE_ID, "aggregates", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_pipe_column[] = {
    {offsetof(SyntaqlitePerfettoPipeColumn, qualifier), SYNTAQLITE_FIELD_SPAN, "qualifier", NULL, 0},
    {offsetof(SyntaqlitePerfettoPipeColumn, name), SYNTAQLITE_FIELD_SPAN, "name", NULL, 0},
    {offsetof(SyntaqlitePerfettoPipeColumn, alias), SYNTAQLITE_FIELD_SPAN, "alias", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_pipe_select[] = {
    {offsetof(SyntaqlitePerfettoPipeSelect, columns), SYNTAQLITE_FIELD_NODE_ID, "columns", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_per_column[] = {
    {offsetof(SyntaqlitePerfettoPerColumn, name), SYNTAQLITE_FIELD_SPAN, "name", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_interval_intersection[] = {
    {offsetof(SyntaqlitePerfettoIntervalIntersection, operands), SYNTAQLITE_FIELD_NODE_ID, "operands", NULL, 0},
    {offsetof(SyntaqlitePerfettoIntervalIntersection, per), SYNTAQLITE_FIELD_NODE_ID, "per", NULL, 0},
};

static const SyntaqliteFieldMeta field_meta_perfetto_pipeline[] = {
    {offsetof(SyntaqlitePerfettoPipeline, from), SYNTAQLITE_FIELD_NODE_ID, "from", NULL, 0},
    {offsetof(SyntaqlitePerfettoPipeline, intersection), SYNTAQLITE_FIELD_NODE_ID, "intersection", NULL, 0},
    {offsetof(SyntaqlitePerfettoPipeline, stages), SYNTAQLITE_FIELD_NODE_ID, "stages", NULL, 0},
};

static const SyntaqliteFieldRangeMeta range_meta_aggregate_function_call[] = {
    {offsetof(SyntaqliteAggregateFunctionCall, func_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_ordered_set_function_call[] = {
    {offsetof(SyntaqliteOrderedSetFunctionCall, func_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_cast_expr[] = {
    {offsetof(SyntaqliteCastExpr, type_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_column_ref[] = {
    {offsetof(SyntaqliteColumnRef, column), 1},
    {offsetof(SyntaqliteColumnRef, table), 1},
    {offsetof(SyntaqliteColumnRef, schema), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_foreign_key_option[] = {
    {offsetof(SyntaqliteForeignKeyOption, match_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_foreign_key_clause[] = {
    {offsetof(SyntaqliteForeignKeyClause, ref_table), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_column_constraint[] = {
    {offsetof(SyntaqliteColumnConstraint, collation_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_constraint_name_declaration[] = {
    {offsetof(SyntaqliteConstraintNameDeclaration, name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_column_def[] = {
    {offsetof(SyntaqliteColumnDef, type_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_table_stmt[] = {
    {offsetof(SyntaqliteCreateTableStmt, table_name), 1},
    {offsetof(SyntaqliteCreateTableStmt, schema), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_cte_definition[] = {
    {offsetof(SyntaqliteCteDefinition, cte_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_delete_stmt[] = {
    {offsetof(SyntaqliteDeleteStmt, index_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_set_clause[] = {
    {offsetof(SyntaqliteSetClause, column), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_update_stmt[] = {
    {offsetof(SyntaqliteUpdateStmt, index_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_literal[] = {
    {offsetof(SyntaqliteLiteral, source), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_ident_name[] = {
    {offsetof(SyntaqliteIdentName, source), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_error[] = {
    {offsetof(SyntaqliteError, source), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_function_call[] = {
    {offsetof(SyntaqliteFunctionCall, func_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_variable[] = {
    {offsetof(SyntaqliteVariable, source), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_collate_expr[] = {
    {offsetof(SyntaqliteCollateExpr, collation), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_transaction_stmt[] = {
    {offsetof(SyntaqliteTransactionStmt, name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_savepoint_stmt[] = {
    {offsetof(SyntaqliteSavepointStmt, transaction_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_table_ref[] = {
    {offsetof(SyntaqliteTableRef, table_name), 1},
    {offsetof(SyntaqliteTableRef, schema), 1},
    {offsetof(SyntaqliteTableRef, index_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_trigger_stmt[] = {
    {offsetof(SyntaqliteCreateTriggerStmt, trigger_name), 1},
    {offsetof(SyntaqliteCreateTriggerStmt, schema), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_virtual_table_stmt[] = {
    {offsetof(SyntaqliteCreateVirtualTableStmt, table_name), 1},
    {offsetof(SyntaqliteCreateVirtualTableStmt, schema), 1},
    {offsetof(SyntaqliteCreateVirtualTableStmt, module_name), 1},
    {offsetof(SyntaqliteCreateVirtualTableStmt, module_args), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_pragma_stmt[] = {
    {offsetof(SyntaqlitePragmaStmt, pragma_name), 1},
    {offsetof(SyntaqlitePragmaStmt, schema), 1},
    {offsetof(SyntaqlitePragmaStmt, value), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_analyze_or_reindex_stmt[] = {
    {offsetof(SyntaqliteAnalyzeOrReindexStmt, target_name), 1},
    {offsetof(SyntaqliteAnalyzeOrReindexStmt, schema), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_vacuum_stmt[] = {
    {offsetof(SyntaqliteVacuumStmt, schema), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_index_stmt[] = {
    {offsetof(SyntaqliteCreateIndexStmt, index_name), 1},
    {offsetof(SyntaqliteCreateIndexStmt, schema), 1},
    {offsetof(SyntaqliteCreateIndexStmt, table_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_view_stmt[] = {
    {offsetof(SyntaqliteCreateViewStmt, view_name), 1},
    {offsetof(SyntaqliteCreateViewStmt, schema), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_window_def[] = {
    {offsetof(SyntaqliteWindowDef, ref_window_name), 1},
    {offsetof(SyntaqliteWindowDef, base_window_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_named_window_def[] = {
    {offsetof(SyntaqliteNamedWindowDef, window_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_filter_over[] = {
    {offsetof(SyntaqliteFilterOver, over_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_arg_def[] = {
    {offsetof(SyntaqlitePerfettoArgDef, arg_type), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_macro_arg[] = {
    {offsetof(SyntaqlitePerfettoMacroArg, arg_name), 1},
    {offsetof(SyntaqlitePerfettoMacroArg, arg_type), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_indexed_column[] = {
    {offsetof(SyntaqlitePerfettoIndexedColumn, column_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_return_type[] = {
    {offsetof(SyntaqlitePerfettoReturnType, scalar_type), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_table_impl[] = {
    {offsetof(SyntaqlitePerfettoTableImpl, name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_perfetto_table_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoTableStmt, table_name), 1},
    {offsetof(SyntaqliteCreatePerfettoTableStmt, select_span), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_perfetto_view_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoViewStmt, view_name), 1},
    {offsetof(SyntaqliteCreatePerfettoViewStmt, select_span), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_perfetto_function_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoFunctionStmt, function_name), 1},
    {offsetof(SyntaqliteCreatePerfettoFunctionStmt, select_span), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_perfetto_delegating_function_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoDelegatingFunctionStmt, function_name), 1},
    {offsetof(SyntaqliteCreatePerfettoDelegatingFunctionStmt, delegate_to), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_perfetto_index_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoIndexStmt, index_name), 1},
    {offsetof(SyntaqliteCreatePerfettoIndexStmt, table_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_create_perfetto_macro_stmt[] = {
    {offsetof(SyntaqliteCreatePerfettoMacroStmt, macro_name), 1},
    {offsetof(SyntaqliteCreatePerfettoMacroStmt, return_type), 1},
    {offsetof(SyntaqliteCreatePerfettoMacroStmt, body), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_include_perfetto_module_stmt[] = {
    {offsetof(SyntaqliteIncludePerfettoModuleStmt, module_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_drop_perfetto_index_stmt[] = {
    {offsetof(SyntaqliteDropPerfettoIndexStmt, index_name), 1},
    {offsetof(SyntaqliteDropPerfettoIndexStmt, table_name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_pragma_stmt[] = {
    {offsetof(SyntaqlitePerfettoPragmaStmt, name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_pipe_source[] = {
    {offsetof(SyntaqlitePerfettoPipeSource, table_name), 1},
    {offsetof(SyntaqlitePerfettoPipeSource, schema), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_tree_aggregate[] = {
    {offsetof(SyntaqlitePerfettoTreeAggregate, name), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_pipe_column[] = {
    {offsetof(SyntaqlitePerfettoPipeColumn, qualifier), 1},
    {offsetof(SyntaqlitePerfettoPipeColumn, name), 1},
    {offsetof(SyntaqlitePerfettoPipeColumn, alias), 1},
};

static const SyntaqliteFieldRangeMeta range_meta_perfetto_per_column[] = {
    {offsetof(SyntaqlitePerfettoPerColumn, name), 1},
};

// ============ Node Names ============

static const char* const ast_meta_node_names[] = {
    "Null",
    "AggregateFunctionCall",
    "OrderedSetFunctionCall",
    "CastExpr",
    "ColumnRef",
    "CompoundSelect",
    "SubqueryExpr",
    "ExistsExpr",
    "InExpr",
    "IsExpr",
    "BetweenExpr",
    "LikeExpr",
    "CaseExpr",
    "CaseWhen",
    "CaseWhenList",
    "ForeignKeyOption",
    "ForeignKeyOptionList",
    "ForeignKeyClause",
    "ColumnConstraint",
    "ConstraintNameDeclaration",
    "ColumnConstraintList",
    "ColumnDef",
    "ColumnDefList",
    "TableConstraint",
    "TableConstraintGroup",
    "TableConstraintList",
    "CreateTableStmt",
    "CteDefinition",
    "CteList",
    "WithClause",
    "UpsertClause",
    "UpsertClauseList",
    "DeleteStmt",
    "SetClause",
    "SetClauseList",
    "UpdateStmt",
    "InsertStmt",
    "BinaryExpr",
    "UnaryExpr",
    "Literal",
    "ParenExpr",
    "IdentName",
    "Error",
    "RowValue",
    "ExprList",
    "FunctionCall",
    "Variable",
    "CollateExpr",
    "RaiseExpr",
    "QualifiedName",
    "DropStmt",
    "AlterTableStmt",
    "TransactionStmt",
    "SavepointStmt",
    "ResultColumn",
    "ResultColumnList",
    "SelectStmt",
    "OrderingTerm",
    "OrderByList",
    "LimitClause",
    "JoinModifier",
    "JoinModifierList",
    "TableRef",
    "SubqueryTableSource",
    "ParenTableSource",
    "JoinClause",
    "JoinPrefix",
    "TriggerEvent",
    "TriggerCmdList",
    "CreateTriggerStmt",
    "CreateVirtualTableStmt",
    "PragmaStmt",
    "AnalyzeOrReindexStmt",
    "AttachStmt",
    "DetachStmt",
    "VacuumStmt",
    "ExplainStmt",
    "CreateIndexStmt",
    "CreateViewStmt",
    "ValuesRowList",
    "ValuesClause",
    "FrameBound",
    "FrameSpec",
    "WindowDef",
    "WindowDefList",
    "NamedWindowDef",
    "NamedWindowDefList",
    "FilterOver",
    "PerfettoArgDef",
    "PerfettoArgDefList",
    "PerfettoMacroArg",
    "PerfettoMacroArgList",
    "PerfettoIndexedColumn",
    "PerfettoIndexedColumnList",
    "PerfettoReturnType",
    "PerfettoTableImpl",
    "CreatePerfettoTableStmt",
    "CreatePerfettoViewStmt",
    "CreatePerfettoFunctionStmt",
    "CreatePerfettoDelegatingFunctionStmt",
    "CreatePerfettoIndexStmt",
    "CreatePerfettoMacroStmt",
    "IncludePerfettoModuleStmt",
    "DropPerfettoIndexStmt",
    "PerfettoPragmaStmt",
    "PerfettoPipeSource",
    "PerfettoTreeAggregate",
    "PerfettoTreeAggregateList",
    "PerfettoTreeAccumulate",
    "PerfettoPipeColumn",
    "PerfettoPipeColumnList",
    "PerfettoPipeSelect",
    "PerfettoPipeStageList",
    "PerfettoPipeSourceList",
    "PerfettoPerColumn",
    "PerfettoPerColumnList",
    "PerfettoIntervalIntersection",
    "PerfettoPipeline",
};

// ============ Field Meta Dispatch ============

static const SyntaqliteFieldMeta* const ast_meta_field_meta[] = {
    NULL, /* Null */
    field_meta_aggregate_function_call, /* AggregateFunctionCall */
    field_meta_ordered_set_function_call, /* OrderedSetFunctionCall */
    field_meta_cast_expr, /* CastExpr */
    field_meta_column_ref, /* ColumnRef */
    field_meta_compound_select, /* CompoundSelect */
    field_meta_subquery_expr, /* SubqueryExpr */
    field_meta_exists_expr, /* ExistsExpr */
    field_meta_in_expr, /* InExpr */
    field_meta_is_expr, /* IsExpr */
    field_meta_between_expr, /* BetweenExpr */
    field_meta_like_expr, /* LikeExpr */
    field_meta_case_expr, /* CaseExpr */
    field_meta_case_when, /* CaseWhen */
    NULL, /* CaseWhenList */
    field_meta_foreign_key_option, /* ForeignKeyOption */
    NULL, /* ForeignKeyOptionList */
    field_meta_foreign_key_clause, /* ForeignKeyClause */
    field_meta_column_constraint, /* ColumnConstraint */
    field_meta_constraint_name_declaration, /* ConstraintNameDeclaration */
    NULL, /* ColumnConstraintList */
    field_meta_column_def, /* ColumnDef */
    NULL, /* ColumnDefList */
    field_meta_table_constraint, /* TableConstraint */
    NULL, /* TableConstraintGroup */
    NULL, /* TableConstraintList */
    field_meta_create_table_stmt, /* CreateTableStmt */
    field_meta_cte_definition, /* CteDefinition */
    NULL, /* CteList */
    field_meta_with_clause, /* WithClause */
    field_meta_upsert_clause, /* UpsertClause */
    NULL, /* UpsertClauseList */
    field_meta_delete_stmt, /* DeleteStmt */
    field_meta_set_clause, /* SetClause */
    NULL, /* SetClauseList */
    field_meta_update_stmt, /* UpdateStmt */
    field_meta_insert_stmt, /* InsertStmt */
    field_meta_binary_expr, /* BinaryExpr */
    field_meta_unary_expr, /* UnaryExpr */
    field_meta_literal, /* Literal */
    field_meta_paren_expr, /* ParenExpr */
    field_meta_ident_name, /* IdentName */
    field_meta_error, /* Error */
    field_meta_row_value, /* RowValue */
    NULL, /* ExprList */
    field_meta_function_call, /* FunctionCall */
    field_meta_variable, /* Variable */
    field_meta_collate_expr, /* CollateExpr */
    field_meta_raise_expr, /* RaiseExpr */
    field_meta_qualified_name, /* QualifiedName */
    field_meta_drop_stmt, /* DropStmt */
    field_meta_alter_table_stmt, /* AlterTableStmt */
    field_meta_transaction_stmt, /* TransactionStmt */
    field_meta_savepoint_stmt, /* SavepointStmt */
    field_meta_result_column, /* ResultColumn */
    NULL, /* ResultColumnList */
    field_meta_select_stmt, /* SelectStmt */
    field_meta_ordering_term, /* OrderingTerm */
    NULL, /* OrderByList */
    field_meta_limit_clause, /* LimitClause */
    field_meta_join_modifier, /* JoinModifier */
    NULL, /* JoinModifierList */
    field_meta_table_ref, /* TableRef */
    field_meta_subquery_table_source, /* SubqueryTableSource */
    field_meta_paren_table_source, /* ParenTableSource */
    field_meta_join_clause, /* JoinClause */
    field_meta_join_prefix, /* JoinPrefix */
    field_meta_trigger_event, /* TriggerEvent */
    NULL, /* TriggerCmdList */
    field_meta_create_trigger_stmt, /* CreateTriggerStmt */
    field_meta_create_virtual_table_stmt, /* CreateVirtualTableStmt */
    field_meta_pragma_stmt, /* PragmaStmt */
    field_meta_analyze_or_reindex_stmt, /* AnalyzeOrReindexStmt */
    field_meta_attach_stmt, /* AttachStmt */
    field_meta_detach_stmt, /* DetachStmt */
    field_meta_vacuum_stmt, /* VacuumStmt */
    field_meta_explain_stmt, /* ExplainStmt */
    field_meta_create_index_stmt, /* CreateIndexStmt */
    field_meta_create_view_stmt, /* CreateViewStmt */
    NULL, /* ValuesRowList */
    field_meta_values_clause, /* ValuesClause */
    field_meta_frame_bound, /* FrameBound */
    field_meta_frame_spec, /* FrameSpec */
    field_meta_window_def, /* WindowDef */
    NULL, /* WindowDefList */
    field_meta_named_window_def, /* NamedWindowDef */
    NULL, /* NamedWindowDefList */
    field_meta_filter_over, /* FilterOver */
    field_meta_perfetto_arg_def, /* PerfettoArgDef */
    NULL, /* PerfettoArgDefList */
    field_meta_perfetto_macro_arg, /* PerfettoMacroArg */
    NULL, /* PerfettoMacroArgList */
    field_meta_perfetto_indexed_column, /* PerfettoIndexedColumn */
    NULL, /* PerfettoIndexedColumnList */
    field_meta_perfetto_return_type, /* PerfettoReturnType */
    field_meta_perfetto_table_impl, /* PerfettoTableImpl */
    field_meta_create_perfetto_table_stmt, /* CreatePerfettoTableStmt */
    field_meta_create_perfetto_view_stmt, /* CreatePerfettoViewStmt */
    field_meta_create_perfetto_function_stmt, /* CreatePerfettoFunctionStmt */
    field_meta_create_perfetto_delegating_function_stmt, /* CreatePerfettoDelegatingFunctionStmt */
    field_meta_create_perfetto_index_stmt, /* CreatePerfettoIndexStmt */
    field_meta_create_perfetto_macro_stmt, /* CreatePerfettoMacroStmt */
    field_meta_include_perfetto_module_stmt, /* IncludePerfettoModuleStmt */
    field_meta_drop_perfetto_index_stmt, /* DropPerfettoIndexStmt */
    field_meta_perfetto_pragma_stmt, /* PerfettoPragmaStmt */
    field_meta_perfetto_pipe_source, /* PerfettoPipeSource */
    field_meta_perfetto_tree_aggregate, /* PerfettoTreeAggregate */
    NULL, /* PerfettoTreeAggregateList */
    field_meta_perfetto_tree_accumulate, /* PerfettoTreeAccumulate */
    field_meta_perfetto_pipe_column, /* PerfettoPipeColumn */
    NULL, /* PerfettoPipeColumnList */
    field_meta_perfetto_pipe_select, /* PerfettoPipeSelect */
    NULL, /* PerfettoPipeStageList */
    NULL, /* PerfettoPipeSourceList */
    field_meta_perfetto_per_column, /* PerfettoPerColumn */
    NULL, /* PerfettoPerColumnList */
    field_meta_perfetto_interval_intersection, /* PerfettoIntervalIntersection */
    field_meta_perfetto_pipeline, /* PerfettoPipeline */
};

static const uint8_t ast_meta_field_meta_counts[] = {
    0, /* Null */
    6, /* AggregateFunctionCall */
    6, /* OrderedSetFunctionCall */
    2, /* CastExpr */
    3, /* ColumnRef */
    5, /* CompoundSelect */
    1, /* SubqueryExpr */
    1, /* ExistsExpr */
    4, /* InExpr */
    3, /* IsExpr */
    4, /* BetweenExpr */
    5, /* LikeExpr */
    3, /* CaseExpr */
    2, /* CaseWhen */
    0, /* CaseWhenList */
    3, /* ForeignKeyOption */
    0, /* ForeignKeyOptionList */
    5, /* ForeignKeyClause */
    14, /* ColumnConstraint */
    1, /* ConstraintNameDeclaration */
    0, /* ColumnConstraintList */
    3, /* ColumnDef */
    0, /* ColumnDefList */
    7, /* TableConstraint */
    0, /* TableConstraintGroup */
    0, /* TableConstraintList */
    8, /* CreateTableStmt */
    4, /* CteDefinition */
    0, /* CteList */
    3, /* WithClause */
    5, /* UpsertClause */
    0, /* UpsertClauseList */
    9, /* DeleteStmt */
    3, /* SetClause */
    0, /* SetClauseList */
    12, /* UpdateStmt */
    9, /* InsertStmt */
    3, /* BinaryExpr */
    2, /* UnaryExpr */
    2, /* Literal */
    1, /* ParenExpr */
    1, /* IdentName */
    1, /* Error */
    1, /* RowValue */
    0, /* ExprList */
    5, /* FunctionCall */
    1, /* Variable */
    2, /* CollateExpr */
    2, /* RaiseExpr */
    2, /* QualifiedName */
    3, /* DropStmt */
    6, /* AlterTableStmt */
    4, /* TransactionStmt */
    5, /* SavepointStmt */
    4, /* ResultColumn */
    0, /* ResultColumnList */
    9, /* SelectStmt */
    3, /* OrderingTerm */
    0, /* OrderByList */
    3, /* LimitClause */
    1, /* JoinModifier */
    0, /* JoinModifierList */
    8, /* TableRef */
    3, /* SubqueryTableSource */
    3, /* ParenTableSource */
    6, /* JoinClause */
    3, /* JoinPrefix */
    2, /* TriggerEvent */
    0, /* TriggerCmdList */
    10, /* CreateTriggerStmt */
    6, /* CreateVirtualTableStmt */
    4, /* PragmaStmt */
    3, /* AnalyzeOrReindexStmt */
    4, /* AttachStmt */
    2, /* DetachStmt */
    2, /* VacuumStmt */
    2, /* ExplainStmt */
    7, /* CreateIndexStmt */
    6, /* CreateViewStmt */
    0, /* ValuesRowList */
    1, /* ValuesClause */
    2, /* FrameBound */
    4, /* FrameSpec */
    5, /* WindowDef */
    0, /* WindowDefList */
    2, /* NamedWindowDef */
    0, /* NamedWindowDefList */
    3, /* FilterOver */
    3, /* PerfettoArgDef */
    0, /* PerfettoArgDefList */
    2, /* PerfettoMacroArg */
    0, /* PerfettoMacroArgList */
    1, /* PerfettoIndexedColumn */
    0, /* PerfettoIndexedColumnList */
    3, /* PerfettoReturnType */
    1, /* PerfettoTableImpl */
    7, /* CreatePerfettoTableStmt */
    5, /* CreatePerfettoViewStmt */
    6, /* CreatePerfettoFunctionStmt */
    5, /* CreatePerfettoDelegatingFunctionStmt */
    4, /* CreatePerfettoIndexStmt */
    5, /* CreatePerfettoMacroStmt */
    1, /* IncludePerfettoModuleStmt */
    2, /* DropPerfettoIndexStmt */
    2, /* PerfettoPragmaStmt */
    5, /* PerfettoPipeSource */
    2, /* PerfettoTreeAggregate */
    0, /* PerfettoTreeAggregateList */
    2, /* PerfettoTreeAccumulate */
    3, /* PerfettoPipeColumn */
    0, /* PerfettoPipeColumnList */
    1, /* PerfettoPipeSelect */
    0, /* PerfettoPipeStageList */
    0, /* PerfettoPipeSourceList */
    1, /* PerfettoPerColumn */
    0, /* PerfettoPerColumnList */
    2, /* PerfettoIntervalIntersection */
    3, /* PerfettoPipeline */
};

// ============ List Tags ============

static const uint8_t ast_meta_list_tags[] = {
    0, /* Null */
    0, /* AggregateFunctionCall */
    0, /* OrderedSetFunctionCall */
    0, /* CastExpr */
    0, /* ColumnRef */
    0, /* CompoundSelect */
    0, /* SubqueryExpr */
    0, /* ExistsExpr */
    0, /* InExpr */
    0, /* IsExpr */
    0, /* BetweenExpr */
    0, /* LikeExpr */
    0, /* CaseExpr */
    0, /* CaseWhen */
    1, /* CaseWhenList */
    0, /* ForeignKeyOption */
    1, /* ForeignKeyOptionList */
    0, /* ForeignKeyClause */
    0, /* ColumnConstraint */
    0, /* ConstraintNameDeclaration */
    1, /* ColumnConstraintList */
    0, /* ColumnDef */
    1, /* ColumnDefList */
    0, /* TableConstraint */
    1, /* TableConstraintGroup */
    1, /* TableConstraintList */
    0, /* CreateTableStmt */
    0, /* CteDefinition */
    1, /* CteList */
    0, /* WithClause */
    0, /* UpsertClause */
    1, /* UpsertClauseList */
    0, /* DeleteStmt */
    0, /* SetClause */
    1, /* SetClauseList */
    0, /* UpdateStmt */
    0, /* InsertStmt */
    0, /* BinaryExpr */
    0, /* UnaryExpr */
    0, /* Literal */
    0, /* ParenExpr */
    0, /* IdentName */
    0, /* Error */
    0, /* RowValue */
    1, /* ExprList */
    0, /* FunctionCall */
    0, /* Variable */
    0, /* CollateExpr */
    0, /* RaiseExpr */
    0, /* QualifiedName */
    0, /* DropStmt */
    0, /* AlterTableStmt */
    0, /* TransactionStmt */
    0, /* SavepointStmt */
    0, /* ResultColumn */
    1, /* ResultColumnList */
    0, /* SelectStmt */
    0, /* OrderingTerm */
    1, /* OrderByList */
    0, /* LimitClause */
    0, /* JoinModifier */
    1, /* JoinModifierList */
    0, /* TableRef */
    0, /* SubqueryTableSource */
    0, /* ParenTableSource */
    0, /* JoinClause */
    0, /* JoinPrefix */
    0, /* TriggerEvent */
    1, /* TriggerCmdList */
    0, /* CreateTriggerStmt */
    0, /* CreateVirtualTableStmt */
    0, /* PragmaStmt */
    0, /* AnalyzeOrReindexStmt */
    0, /* AttachStmt */
    0, /* DetachStmt */
    0, /* VacuumStmt */
    0, /* ExplainStmt */
    0, /* CreateIndexStmt */
    0, /* CreateViewStmt */
    1, /* ValuesRowList */
    0, /* ValuesClause */
    0, /* FrameBound */
    0, /* FrameSpec */
    0, /* WindowDef */
    1, /* WindowDefList */
    0, /* NamedWindowDef */
    1, /* NamedWindowDefList */
    0, /* FilterOver */
    0, /* PerfettoArgDef */
    1, /* PerfettoArgDefList */
    0, /* PerfettoMacroArg */
    1, /* PerfettoMacroArgList */
    0, /* PerfettoIndexedColumn */
    1, /* PerfettoIndexedColumnList */
    0, /* PerfettoReturnType */
    0, /* PerfettoTableImpl */
    0, /* CreatePerfettoTableStmt */
    0, /* CreatePerfettoViewStmt */
    0, /* CreatePerfettoFunctionStmt */
    0, /* CreatePerfettoDelegatingFunctionStmt */
    0, /* CreatePerfettoIndexStmt */
    0, /* CreatePerfettoMacroStmt */
    0, /* IncludePerfettoModuleStmt */
    0, /* DropPerfettoIndexStmt */
    0, /* PerfettoPragmaStmt */
    0, /* PerfettoPipeSource */
    0, /* PerfettoTreeAggregate */
    1, /* PerfettoTreeAggregateList */
    0, /* PerfettoTreeAccumulate */
    0, /* PerfettoPipeColumn */
    1, /* PerfettoPipeColumnList */
    0, /* PerfettoPipeSelect */
    1, /* PerfettoPipeStageList */
    1, /* PerfettoPipeSourceList */
    0, /* PerfettoPerColumn */
    1, /* PerfettoPerColumnList */
    0, /* PerfettoIntervalIntersection */
    0, /* PerfettoPipeline */
};

// ============ Range Meta Dispatch ============

static const SyntaqliteRangeMetaEntry ast_meta_range_meta[] = {
    {NULL, 0}, /* Null */
    {range_meta_aggregate_function_call, 1}, /* AggregateFunctionCall */
    {range_meta_ordered_set_function_call, 1}, /* OrderedSetFunctionCall */
    {range_meta_cast_expr, 1}, /* CastExpr */
    {range_meta_column_ref, 3}, /* ColumnRef */
    {NULL, 0}, /* CompoundSelect */
    {NULL, 0}, /* SubqueryExpr */
    {NULL, 0}, /* ExistsExpr */
    {NULL, 0}, /* InExpr */
    {NULL, 0}, /* IsExpr */
    {NULL, 0}, /* BetweenExpr */
    {NULL, 0}, /* LikeExpr */
    {NULL, 0}, /* CaseExpr */
    {NULL, 0}, /* CaseWhen */
    {NULL, 0}, /* CaseWhenList */
    {range_meta_foreign_key_option, 1}, /* ForeignKeyOption */
    {NULL, 0}, /* ForeignKeyOptionList */
    {range_meta_foreign_key_clause, 1}, /* ForeignKeyClause */
    {range_meta_column_constraint, 1}, /* ColumnConstraint */
    {range_meta_constraint_name_declaration, 1}, /* ConstraintNameDeclaration */
    {NULL, 0}, /* ColumnConstraintList */
    {range_meta_column_def, 1}, /* ColumnDef */
    {NULL, 0}, /* ColumnDefList */
    {NULL, 0}, /* TableConstraint */
    {NULL, 0}, /* TableConstraintGroup */
    {NULL, 0}, /* TableConstraintList */
    {range_meta_create_table_stmt, 2}, /* CreateTableStmt */
    {range_meta_cte_definition, 1}, /* CteDefinition */
    {NULL, 0}, /* CteList */
    {NULL, 0}, /* WithClause */
    {NULL, 0}, /* UpsertClause */
    {NULL, 0}, /* UpsertClauseList */
    {range_meta_delete_stmt, 1}, /* DeleteStmt */
    {range_meta_set_clause, 1}, /* SetClause */
    {NULL, 0}, /* SetClauseList */
    {range_meta_update_stmt, 1}, /* UpdateStmt */
    {NULL, 0}, /* InsertStmt */
    {NULL, 0}, /* BinaryExpr */
    {NULL, 0}, /* UnaryExpr */
    {range_meta_literal, 1}, /* Literal */
    {NULL, 0}, /* ParenExpr */
    {range_meta_ident_name, 1}, /* IdentName */
    {range_meta_error, 1}, /* Error */
    {NULL, 0}, /* RowValue */
    {NULL, 0}, /* ExprList */
    {range_meta_function_call, 1}, /* FunctionCall */
    {range_meta_variable, 1}, /* Variable */
    {range_meta_collate_expr, 1}, /* CollateExpr */
    {NULL, 0}, /* RaiseExpr */
    {NULL, 0}, /* QualifiedName */
    {NULL, 0}, /* DropStmt */
    {NULL, 0}, /* AlterTableStmt */
    {range_meta_transaction_stmt, 1}, /* TransactionStmt */
    {range_meta_savepoint_stmt, 1}, /* SavepointStmt */
    {NULL, 0}, /* ResultColumn */
    {NULL, 0}, /* ResultColumnList */
    {NULL, 0}, /* SelectStmt */
    {NULL, 0}, /* OrderingTerm */
    {NULL, 0}, /* OrderByList */
    {NULL, 0}, /* LimitClause */
    {NULL, 0}, /* JoinModifier */
    {NULL, 0}, /* JoinModifierList */
    {range_meta_table_ref, 3}, /* TableRef */
    {NULL, 0}, /* SubqueryTableSource */
    {NULL, 0}, /* ParenTableSource */
    {NULL, 0}, /* JoinClause */
    {NULL, 0}, /* JoinPrefix */
    {NULL, 0}, /* TriggerEvent */
    {NULL, 0}, /* TriggerCmdList */
    {range_meta_create_trigger_stmt, 2}, /* CreateTriggerStmt */
    {range_meta_create_virtual_table_stmt, 4}, /* CreateVirtualTableStmt */
    {range_meta_pragma_stmt, 3}, /* PragmaStmt */
    {range_meta_analyze_or_reindex_stmt, 2}, /* AnalyzeOrReindexStmt */
    {NULL, 0}, /* AttachStmt */
    {NULL, 0}, /* DetachStmt */
    {range_meta_vacuum_stmt, 1}, /* VacuumStmt */
    {NULL, 0}, /* ExplainStmt */
    {range_meta_create_index_stmt, 3}, /* CreateIndexStmt */
    {range_meta_create_view_stmt, 2}, /* CreateViewStmt */
    {NULL, 0}, /* ValuesRowList */
    {NULL, 0}, /* ValuesClause */
    {NULL, 0}, /* FrameBound */
    {NULL, 0}, /* FrameSpec */
    {range_meta_window_def, 2}, /* WindowDef */
    {NULL, 0}, /* WindowDefList */
    {range_meta_named_window_def, 1}, /* NamedWindowDef */
    {NULL, 0}, /* NamedWindowDefList */
    {range_meta_filter_over, 1}, /* FilterOver */
    {range_meta_perfetto_arg_def, 1}, /* PerfettoArgDef */
    {NULL, 0}, /* PerfettoArgDefList */
    {range_meta_perfetto_macro_arg, 2}, /* PerfettoMacroArg */
    {NULL, 0}, /* PerfettoMacroArgList */
    {range_meta_perfetto_indexed_column, 1}, /* PerfettoIndexedColumn */
    {NULL, 0}, /* PerfettoIndexedColumnList */
    {range_meta_perfetto_return_type, 1}, /* PerfettoReturnType */
    {range_meta_perfetto_table_impl, 1}, /* PerfettoTableImpl */
    {range_meta_create_perfetto_table_stmt, 2}, /* CreatePerfettoTableStmt */
    {range_meta_create_perfetto_view_stmt, 2}, /* CreatePerfettoViewStmt */
    {range_meta_create_perfetto_function_stmt, 2}, /* CreatePerfettoFunctionStmt */
    {range_meta_create_perfetto_delegating_function_stmt, 2}, /* CreatePerfettoDelegatingFunctionStmt */
    {range_meta_create_perfetto_index_stmt, 2}, /* CreatePerfettoIndexStmt */
    {range_meta_create_perfetto_macro_stmt, 3}, /* CreatePerfettoMacroStmt */
    {range_meta_include_perfetto_module_stmt, 1}, /* IncludePerfettoModuleStmt */
    {range_meta_drop_perfetto_index_stmt, 2}, /* DropPerfettoIndexStmt */
    {range_meta_perfetto_pragma_stmt, 1}, /* PerfettoPragmaStmt */
    {range_meta_perfetto_pipe_source, 2}, /* PerfettoPipeSource */
    {range_meta_perfetto_tree_aggregate, 1}, /* PerfettoTreeAggregate */
    {NULL, 0}, /* PerfettoTreeAggregateList */
    {NULL, 0}, /* PerfettoTreeAccumulate */
    {range_meta_perfetto_pipe_column, 3}, /* PerfettoPipeColumn */
    {NULL, 0}, /* PerfettoPipeColumnList */
    {NULL, 0}, /* PerfettoPipeSelect */
    {NULL, 0}, /* PerfettoPipeStageList */
    {NULL, 0}, /* PerfettoPipeSourceList */
    {range_meta_perfetto_per_column, 1}, /* PerfettoPerColumn */
    {NULL, 0}, /* PerfettoPerColumnList */
    {NULL, 0}, /* PerfettoIntervalIntersection */
    {NULL, 0}, /* PerfettoPipeline */
};


#endif  /* SYNTAQLITE_DIALECT_META_H */
/* ======== end: csrc/dialect_meta.h ======== */

/* ======== begin: csrc/dialect_tokens.h ======== */
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT

#define TOKEN_TYPE_COUNT 202

static const uint8_t token_categories[202] = {
    0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,0,1,1,1,1,0,
    0,5,5,5,5,5,5,1,2,0,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,0,5,5,5,
    5,5,5,5,5,5,5,5,1,5,1,1,3,1,4,4,
    6,6,1,6,1,1,6,6,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,3,4,8,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,0,9,0,0,1,0,
    0,0,0,0,0,0,6,0,0,0,7,0,0,1,1,1,
    1,1,1,1,1,1,1,1,1,1,
};
/* ======== end: csrc/dialect_tokens.h ======== */

/* ======== begin: csrc/dialect_fmt.h ======== */
#ifndef SYNTAQLITE_PERFETTO_DIALECT_FMT_H
#define SYNTAQLITE_PERFETTO_DIALECT_FMT_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT


#include <stdint.h>

static const uint8_t perfetto_fmt_string_data[] = {
    0x28,0x44,0x49,0x53,0x54,0x49,0x4e,0x43,0x54,0x20,0x41,0x4c,0x4c,0x4f,0x52,0x44,
    0x45,0x52,0x42,0x59,0x29,0x46,0x49,0x4c,0x54,0x45,0x52,0x57,0x48,0x45,0x52,0x45,
    0x4f,0x56,0x45,0x52,0x57,0x49,0x54,0x48,0x49,0x4e,0x47,0x52,0x4f,0x55,0x50,0x43,
    0x41,0x53,0x54,0x41,0x53,0x2e,0x55,0x4e,0x49,0x4f,0x4e,0x49,0x4e,0x54,0x45,0x52,
    0x53,0x45,0x43,0x54,0x45,0x58,0x43,0x45,0x50,0x54,0x4c,0x49,0x4d,0x49,0x54,0x45,
    0x58,0x49,0x53,0x54,0x53,0x4e,0x4f,0x54,0x49,0x4e,0x49,0x53,0x4e,0x55,0x4c,0x4c,
    0x4e,0x4f,0x54,0x4e,0x55,0x4c,0x4c,0x4e,0x55,0x4c,0x4c,0x49,0x53,0x46,0x52,0x4f,
    0x4d,0x42,0x45,0x54,0x57,0x45,0x45,0x4e,0x41,0x4e,0x44,0x4c,0x49,0x4b,0x45,0x47,
    0x4c,0x4f,0x42,0x4d,0x41,0x54,0x43,0x48,0x52,0x45,0x47,0x45,0x58,0x50,0x45,0x53,
    0x43,0x41,0x50,0x45,0x43,0x41,0x53,0x45,0x45,0x4c,0x53,0x45,0x45,0x4e,0x44,0x57,
    0x48,0x45,0x4e,0x54,0x48,0x45,0x4e,0x4f,0x4e,0x44,0x45,0x4c,0x45,0x54,0x45,0x55,
    0x50,0x44,0x41,0x54,0x45,0x49,0x4e,0x53,0x45,0x52,0x54,0x4e,0x4f,0x41,0x43,0x54,
    0x49,0x4f,0x4e,0x53,0x45,0x54,0x44,0x45,0x46,0x41,0x55,0x4c,0x54,0x43,0x41,0x53,
    0x43,0x41,0x44,0x45,0x52,0x45,0x53,0x54,0x52,0x49,0x43,0x54,0x52,0x45,0x46,0x45,
    0x52,0x45,0x4e,0x43,0x45,0x53,0x44,0x45,0x46,0x45,0x52,0x52,0x41,0x42,0x4c,0x45,
    0x49,0x4e,0x49,0x54,0x49,0x41,0x4c,0x4c,0x59,0x44,0x45,0x46,0x45,0x52,0x52,0x45,
    0x44,0x49,0x4d,0x4d,0x45,0x44,0x49,0x41,0x54,0x45,0x50,0x52,0x49,0x4d,0x41,0x52,
    0x59,0x4b,0x45,0x59,0x41,0x53,0x43,0x44,0x45,0x53,0x43,0x43,0x4f,0x4e,0x46,0x4c,
    0x49,0x43,0x54,0x52,0x4f,0x4c,0x4c,0x42,0x41,0x43,0x4b,0x41,0x42,0x4f,0x52,0x54,
    0x46,0x41,0x49,0x4c,0x49,0x47,0x4e,0x4f,0x52,0x45,0x52,0x45,0x50,0x4c,0x41,0x43,
    0x45,0x41,0x55,0x54,0x4f,0x49,0x4e,0x43,0x52,0x45,0x4d,0x45,0x4e,0x54,0x55,0x4e,
    0x49,0x51,0x55,0x45,0x43,0x48,0x45,0x43,0x4b,0x43,0x4f,0x4c,0x4c,0x41,0x54,0x45,
    0x47,0x45,0x4e,0x45,0x52,0x41,0x54,0x45,0x44,0x41,0x4c,0x57,0x41,0x59,0x53,0x56,
    0x49,0x52,0x54,0x55,0x41,0x4c,0x53,0x54,0x4f,0x52,0x45,0x44,0x43,0x4f,0x4e,0x53,
    0x54,0x52,0x41,0x49,0x4e,0x54,0x2c,0x46,0x4f,0x52,0x45,0x49,0x47,0x4e,0x43,0x52,
    0x45,0x41,0x54,0x45,0x54,0x45,0x4d,0x50,0x54,0x45,0x4d,0x50,0x4f,0x52,0x41,0x52,
    0x59,0x54,0x41,0x42,0x4c,0x45,0x49,0x46,0x57,0x49,0x54,0x48,0x4f,0x55,0x54,0x52,
    0x4f,0x57,0x49,0x44,0x53,0x54,0x52,0x49,0x43,0x54,0x4d,0x41,0x54,0x45,0x52,0x49,
    0x41,0x4c,0x49,0x5a,0x45,0x44,0x57,0x49,0x54,0x48,0x52,0x45,0x43,0x55,0x52,0x53,
    0x49,0x56,0x45,0x44,0x4f,0x4e,0x4f,0x54,0x48,0x49,0x4e,0x47,0x49,0x4e,0x44,0x45,
    0x58,0x45,0x44,0x52,0x45,0x54,0x55,0x52,0x4e,0x49,0x4e,0x47,0x3d,0x4f,0x52,0x49,
    0x4e,0x54,0x4f,0x56,0x41,0x4c,0x55,0x45,0x53,0x2b,0x2d,0x2a,0x2f,0x25,0x3c,0x3e,
    0x3c,0x3d,0x3e,0x3d,0x21,0x3d,0x26,0x7c,0x3c,0x3c,0x3e,0x3e,0x7c,0x7c,0x2d,0x3e,
    0x2d,0x3e,0x3e,0x3c,0x3e,0x3d,0x3d,0x7e,0x3c,0x65,0x72,0x72,0x6f,0x72,0x3e,0x52,
    0x41,0x49,0x53,0x45,0x44,0x52,0x4f,0x50,0x49,0x4e,0x44,0x45,0x58,0x56,0x49,0x45,
    0x57,0x54,0x52,0x49,0x47,0x47,0x45,0x52,0x41,0x4c,0x54,0x45,0x52,0x52,0x45,0x4e,
    0x41,0x4d,0x45,0x54,0x4f,0x43,0x4f,0x4c,0x55,0x4d,0x4e,0x41,0x44,0x44,0x42,0x45,
    0x47,0x49,0x4e,0x45,0x58,0x43,0x4c,0x55,0x53,0x49,0x56,0x45,0x43,0x4f,0x4d,0x4d,
    0x49,0x54,0x54,0x52,0x41,0x4e,0x53,0x41,0x43,0x54,0x49,0x4f,0x4e,0x53,0x41,0x56,
    0x45,0x50,0x4f,0x49,0x4e,0x54,0x52,0x45,0x4c,0x45,0x41,0x53,0x45,0x53,0x45,0x4c,
    0x45,0x43,0x54,0x48,0x41,0x56,0x49,0x4e,0x47,0x57,0x49,0x4e,0x44,0x4f,0x57,0x4e,
    0x55,0x4c,0x4c,0x53,0x46,0x49,0x52,0x53,0x54,0x4c,0x41,0x53,0x54,0x4f,0x46,0x46,
    0x53,0x45,0x54,0x4e,0x41,0x54,0x55,0x52,0x41,0x4c,0x4c,0x45,0x46,0x54,0x4f,0x55,
    0x54,0x45,0x52,0x52,0x49,0x47,0x48,0x54,0x46,0x55,0x4c,0x4c,0x49,0x4e,0x4e,0x45,
    0x52,0x43,0x52,0x4f,0x53,0x53,0x55,0x53,0x49,0x4e,0x47,0x4a,0x4f,0x49,0x4e,0x4f,
    0x46,0x3b,0x42,0x45,0x46,0x4f,0x52,0x45,0x41,0x46,0x54,0x45,0x52,0x49,0x4e,0x53,
    0x54,0x45,0x41,0x44,0x46,0x4f,0x52,0x45,0x41,0x43,0x48,0x52,0x4f,0x57,0x50,0x52,
    0x41,0x47,0x4d,0x41,0x52,0x45,0x49,0x4e,0x44,0x45,0x58,0x41,0x4e,0x41,0x4c,0x59,
    0x5a,0x45,0x41,0x54,0x54,0x41,0x43,0x48,0x44,0x41,0x54,0x41,0x42,0x41,0x53,0x45,
    0x44,0x45,0x54,0x41,0x43,0x48,0x56,0x41,0x43,0x55,0x55,0x4d,0x45,0x58,0x50,0x4c,
    0x41,0x49,0x4e,0x51,0x55,0x45,0x52,0x59,0x50,0x4c,0x41,0x4e,0x55,0x4e,0x42,0x4f,
    0x55,0x4e,0x44,0x45,0x44,0x50,0x52,0x45,0x43,0x45,0x44,0x49,0x4e,0x47,0x43,0x55,
    0x52,0x52,0x45,0x4e,0x54,0x46,0x4f,0x4c,0x4c,0x4f,0x57,0x49,0x4e,0x47,0x52,0x41,
    0x4e,0x47,0x45,0x52,0x4f,0x57,0x53,0x47,0x52,0x4f,0x55,0x50,0x53,0x45,0x58,0x43,
    0x4c,0x55,0x44,0x45,0x4f,0x54,0x48,0x45,0x52,0x53,0x54,0x49,0x45,0x53,0x50,0x41,
    0x52,0x54,0x49,0x54,0x49,0x4f,0x4e,0x2e,0x2e,0x2e,0x54,0x41,0x42,0x4c,0x45,0x28,
    0x50,0x45,0x52,0x46,0x45,0x54,0x54,0x4f,0x46,0x55,0x4e,0x43,0x54,0x49,0x4f,0x4e,
    0x52,0x45,0x54,0x55,0x52,0x4e,0x53,0x44,0x45,0x4c,0x45,0x47,0x41,0x54,0x45,0x53,
    0x4d,0x41,0x43,0x52,0x4f,0x49,0x4e,0x43,0x4c,0x55,0x44,0x45,0x4d,0x4f,0x44,0x55,
    0x4c,0x45,0x54,0x52,0x45,0x45,0x41,0x43,0x43,0x55,0x4d,0x55,0x4c,0x41,0x54,0x45,
    0x55,0x50,0x44,0x4f,0x57,0x4e,0x7c,0x3e,0x49,0x4e,0x54,0x45,0x52,0x56,0x41,0x4c,
    0x49,0x4e,0x54,0x45,0x52,0x53,0x45,0x43,0x54,0x49,0x4f,0x4e,0x50,0x45,0x52,
};

static const uint32_t perfetto_fmt_string_offsets[] = {
    0,1,9,10,13,18,20,21,27,32,36,42,47,51,53,54,
    59,68,74,79,85,88,90,96,103,107,109,113,120,123,127,131,
    136,142,148,152,156,159,163,167,169,169,175,181,187,189,195,198,
    205,212,220,230,240,249,257,266,273,276,279,283,291,299,304,308,
    314,321,334,340,345,352,361,367,374,380,390,391,398,404,408,417,
    422,424,431,436,442,454,458,467,469,476,483,492,493,495,499,505,
    506,507,508,509,510,511,512,514,516,518,519,520,522,524,526,528,
    531,533,535,536,543,548,552,557,561,568,573,579,581,587,590,595,
    604,610,621,630,637,643,649,655,660,665,669,675,682,686,691,696,
    700,705,710,715,719,721,722,728,733,740,743,747,750,756,763,770,
    776,784,790,796,803,808,812,821,830,837,846,851,855,861,868,874,
    878,887,890,896,904,912,919,928,933,940,946,950,960,962,966,968,
    976,988,991,
};

static const uint32_t perfetto_fmt_string_count = 194;

static const uint16_t perfetto_fmt_enum_display[] = {
    40,41,42,43,95,96,97,98,99,100,101,102,103,91,104,28,
    92,105,106,107,108,109,110,111,112,113,79,118,119,120,139,140,
    141,142,143,144,145,188,189,
};

static const uint32_t perfetto_fmt_enum_display_count = 39;

static const uint8_t perfetto_fmt_ops[] = {
    1,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    18,1,1,0,3,0,
    0,0,1,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    18,1,4,0,3,0,
    0,0,3,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    10,2,0,0,5,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,3,0,0,6,0,
    0,0,2,0,0,0,
    0,0,4,0,0,0,
    0,0,5,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    12,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    10,4,0,0,15,0,
    6,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,7,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,8,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,5,0,0,5,0,
    0,0,2,0,0,0,
    0,0,9,0,0,0,
    0,0,2,0,0,0,
    2,5,0,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    18,1,1,0,3,0,
    0,0,1,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    18,1,4,0,3,0,
    0,0,3,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    10,2,0,0,5,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,10,0,0,0,
    0,0,11,0,0,0,
    0,0,2,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    0,0,4,0,0,0,
    0,0,5,0,0,0,
    0,0,2,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    10,4,0,0,15,0,
    6,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,7,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,8,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,5,0,0,5,0,
    0,0,2,0,0,0,
    0,0,9,0,0,0,
    0,0,2,0,0,0,
    2,5,0,0,0,0,
    12,0,0,0,0,0,
    0,0,12,0,0,0,
    0,0,0,0,0,0,
    24,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    1,1,0,0,0,0,
    0,0,6,0,0,0,
    20,2,0,0,3,0,
    1,2,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    2,1,0,0,0,0,
    5,0,0,0,0,0,
    19,0,0,0,2,0,
    0,0,15,0,0,0,
    11,0,0,0,13,0,
    19,0,1,0,3,0,
    0,0,15,0,0,0,
    0,0,3,0,0,0,
    11,0,0,0,8,0,
    19,0,2,0,2,0,
    0,0,16,0,0,0,
    11,0,0,0,4,0,
    19,0,3,0,2,0,
    0,0,17,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    5,0,0,0,0,0,
    6,0,0,0,0,0,
    2,2,0,0,0,0,
    10,3,0,0,8,0,
    3,0,0,0,0,0,
    0,0,4,0,0,0,
    0,0,5,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,4,0,0,5,0,
    3,0,0,0,0,0,
    0,0,18,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    0,0,19,0,0,0,
    0,0,2,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    25,2,0,3,0,0,
    17,0,0,0,5,0,
    0,0,2,0,0,0,
    0,0,20,0,0,0,
    0,0,21,0,0,0,
    0,0,2,0,0,0,
    11,0,0,0,4,0,
    0,0,2,0,0,0,
    0,0,21,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    17,1,0,0,2,0,
    2,3,0,0,0,0,
    11,0,0,0,10,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    19,0,2,0,4,0,
    25,1,0,3,0,0,
    0,0,2,0,0,0,
    0,0,22,0,0,0,
    11,0,0,0,51,0,
    19,0,3,0,4,0,
    25,1,0,3,0,0,
    0,0,2,0,0,0,
    0,0,23,0,0,0,
    11,0,0,0,45,0,
    19,0,6,0,5,0,
    25,1,0,3,0,0,
    0,0,2,0,0,0,
    0,0,20,0,0,0,
    0,0,24,0,0,0,
    11,0,0,0,38,0,
    19,0,0,0,6,0,
    25,1,0,3,0,0,
    0,0,2,0,0,0,
    0,0,25,0,0,0,
    0,0,2,0,0,0,
    25,2,0,3,1,0,
    11,0,0,0,30,0,
    19,0,1,0,7,0,
    25,1,0,3,0,0,
    0,0,2,0,0,0,
    0,0,25,0,0,0,
    0,0,20,0,0,0,
    0,0,2,0,0,0,
    25,2,0,3,1,0,
    11,0,0,0,21,0,
    19,0,4,0,9,0,
    25,1,0,3,0,0,
    0,0,2,0,0,0,
    0,0,25,0,0,0,
    0,0,20,0,0,0,
    0,0,1,0,0,0,
    0,0,26,0,0,0,
    0,0,2,0,0,0,
    25,2,0,3,1,0,
    11,0,0,0,10,0,
    19,0,5,0,8,0,
    25,1,0,3,0,0,
    0,0,2,0,0,0,
    0,0,25,0,0,0,
    0,0,1,0,0,0,
    0,0,26,0,0,0,
    0,0,2,0,0,0,
    25,2,0,3,1,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    25,1,0,3,0,0,
    17,0,0,0,5,0,
    0,0,2,0,0,0,
    0,0,20,0,0,0,
    0,0,27,0,0,0,
    0,0,2,0,0,0,
    11,0,0,0,4,0,
    0,0,2,0,0,0,
    0,0,27,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    2,2,0,0,0,0,
    0,0,2,0,0,0,
    0,0,28,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    25,2,0,3,0,0,
    17,0,0,0,4,0,
    0,0,2,0,0,0,
    0,0,20,0,0,0,
    0,0,2,0,0,0,
    11,0,0,0,2,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    19,1,0,0,2,0,
    0,0,29,0,0,0,
    11,0,0,0,12,0,
    19,1,1,0,2,0,
    0,0,30,0,0,0,
    11,0,0,0,8,0,
    19,1,2,0,2,0,
    0,0,31,0,0,0,
    11,0,0,0,4,0,
    19,1,3,0,2,0,
    0,0,32,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    10,4,0,0,5,0,
    0,0,2,0,0,0,
    0,0,33,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,34,0,0,0,
    10,0,0,0,3,0,
    0,0,2,0,0,0,
    24,0,0,0,0,0,
    12,0,0,0,0,0,
    8,0,0,0,0,0,
    10,2,0,0,2,0,
    2,2,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,5,0,
    3,0,0,0,0,0,
    0,0,35,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    9,0,0,0,0,0,
    3,0,0,0,0,0,
    0,0,36,0,0,0,
    7,0,0,0,0,0,
    3,0,0,0,0,0,
    0,0,37,0,0,0,
    0,0,2,0,0,0,
    2,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,38,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    16,0,0,0,0,0,
    19,0,0,0,4,0,
    0,0,31,0,0,0,
    0,0,2,0,0,0,
    1,2,0,0,0,0,
    11,0,0,0,27,0,
    0,0,39,0,0,0,
    0,0,2,0,0,0,
    21,0,0,0,0,0,
    0,0,2,0,0,0,
    19,1,1,0,3,0,
    0,0,44,0,0,0,
    0,0,45,0,0,0,
    11,0,0,0,18,0,
    19,1,2,0,3,0,
    0,0,46,0,0,0,
    0,0,24,0,0,0,
    11,0,0,0,13,0,
    19,1,3,0,3,0,
    0,0,46,0,0,0,
    0,0,47,0,0,0,
    11,0,0,0,8,0,
    19,1,4,0,2,0,
    0,0,48,0,0,0,
    11,0,0,0,4,0,
    19,1,5,0,2,0,
    0,0,49,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,40,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    20,0,0,0,4,0,
    0,0,50,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,10,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,1,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    8,0,0,0,0,0,
    10,2,0,0,3,0,
    3,0,0,0,0,0,
    2,2,0,0,0,0,
    12,0,0,0,0,0,
    19,3,1,0,4,0,
    3,0,0,0,0,0,
    0,0,20,0,0,0,
    0,0,51,0,0,0,
    11,0,0,0,5,0,
    19,3,2,0,3,0,
    3,0,0,0,0,0,
    0,0,51,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    19,4,1,0,4,0,
    0,0,2,0,0,0,
    0,0,52,0,0,0,
    0,0,53,0,0,0,
    11,0,0,0,6,0,
    19,4,2,0,4,0,
    0,0,2,0,0,0,
    0,0,52,0,0,0,
    0,0,54,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    19,0,2,0,49,0,
    0,0,55,0,0,0,
    0,0,56,0,0,0,
    19,2,1,0,3,0,
    0,0,2,0,0,0,
    0,0,57,0,0,0,
    12,0,0,0,0,0,
    19,2,2,0,3,0,
    0,0,2,0,0,0,
    0,0,58,0,0,0,
    12,0,0,0,0,0,
    19,1,1,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,28,0,
    19,1,2,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,21,0,
    19,1,3,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,14,0,
    19,1,4,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,7,0,
    19,1,5,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    17,3,0,0,3,0,
    0,0,2,0,0,0,
    0,0,65,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,205,0,
    19,0,1,0,37,0,
    0,0,20,0,0,0,
    0,0,24,0,0,0,
    19,1,1,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,28,0,
    19,1,2,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,21,0,
    19,1,3,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,14,0,
    19,1,4,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,7,0,
    19,1,5,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,166,0,
    19,0,3,0,36,0,
    0,0,66,0,0,0,
    19,1,1,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,28,0,
    19,1,2,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,21,0,
    19,1,3,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,14,0,
    19,1,4,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,7,0,
    19,1,5,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,128,0,
    19,0,4,0,11,0,
    6,0,0,0,0,0,
    0,0,67,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,11,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    11,0,0,0,115,0,
    19,0,0,0,16,0,
    0,0,47,0,0,0,
    0,0,2,0,0,0,
    17,8,0,0,10,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,10,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    11,0,0,0,2,0,
    2,10,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,97,0,
    19,0,6,0,4,0,
    0,0,68,0,0,0,
    0,0,2,0,0,0,
    1,4,0,0,0,0,
    11,0,0,0,91,0,
    19,0,5,0,2,0,
    2,13,0,0,0,0,
    11,0,0,0,87,0,
    19,0,7,0,25,0,
    17,9,0,0,4,0,
    0,0,69,0,0,0,
    0,0,70,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,12,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    19,5,1,0,3,0,
    0,0,2,0,0,0,
    0,0,71,0,0,0,
    12,0,0,0,0,0,
    19,5,2,0,3,0,
    0,0,2,0,0,0,
    0,0,72,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,60,0,
    19,0,8,0,36,0,
    0,0,24,0,0,0,
    19,1,1,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,28,0,
    19,1,2,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,21,0,
    19,1,3,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,14,0,
    19,1,4,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,7,0,
    19,1,5,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,22,0,
    19,0,9,0,20,0,
    19,6,1,0,3,0,
    0,0,20,0,0,0,
    0,0,51,0,0,0,
    11,0,0,0,4,0,
    19,6,2,0,2,0,
    0,0,51,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    19,7,1,0,4,0,
    0,0,2,0,0,0,
    0,0,52,0,0,0,
    0,0,53,0,0,0,
    11,0,0,0,6,0,
    19,7,2,0,4,0,
    0,0,2,0,0,0,
    0,0,52,0,0,0,
    0,0,54,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,73,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,40,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    2,0,0,0,0,0,
    20,1,0,0,3,0,
    0,0,2,0,0,0,
    1,1,0,0,0,0,
    12,0,0,0,0,0,
    10,2,0,0,5,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    19,0,0,0,46,0,
    6,0,0,0,0,0,
    0,0,55,0,0,0,
    0,0,56,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    19,1,1,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,28,0,
    19,1,2,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,21,0,
    19,1,3,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,14,0,
    19,1,4,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,7,0,
    19,1,5,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,110,0,
    19,0,1,0,45,0,
    6,0,0,0,0,0,
    0,0,66,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    19,1,1,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,28,0,
    19,1,2,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,21,0,
    19,1,3,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,14,0,
    19,1,4,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,7,0,
    19,1,5,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,63,0,
    19,0,2,0,45,0,
    6,0,0,0,0,0,
    0,0,67,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,5,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    19,1,1,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,28,0,
    19,1,2,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,21,0,
    19,1,3,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,14,0,
    19,1,4,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,7,0,
    19,1,5,0,5,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,16,0,
    19,0,3,0,14,0,
    6,0,0,0,0,0,
    0,0,75,0,0,0,
    0,0,56,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,4,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    0,0,2,0,0,0,
    2,6,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,40,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    7,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    19,2,1,0,3,0,
    0,0,2,0,0,0,
    0,0,77,0,0,0,
    11,0,0,0,5,0,
    19,2,2,0,3,0,
    0,0,2,0,0,0,
    0,0,78,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,79,0,0,0,
    17,3,0,0,5,0,
    0,0,2,0,0,0,
    0,0,80,0,0,0,
    0,0,20,0,0,0,
    0,0,19,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    10,5,0,0,13,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,5,0,0,0,0,
    10,6,0,0,4,0,
    0,0,74,0,0,0,
    3,0,0,0,0,0,
    2,6,0,0,0,0,
    12,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    12,0,0,0,0,0,
    10,7,0,0,3,0,
    3,0,0,0,0,0,
    0,0,13,0,0,0,
    12,0,0,0,0,0,
    18,4,1,0,7,0,
    0,0,2,0,0,0,
    0,0,81,0,0,0,
    0,0,82,0,0,0,
    18,4,2,0,2,0,
    0,0,74,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    18,4,2,0,3,0,
    0,0,2,0,0,0,
    0,0,83,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    10,7,0,0,3,0,
    5,0,0,0,0,0,
    2,7,0,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    10,2,0,0,10,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    19,1,1,0,3,0,
    0,0,84,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    19,1,2,0,4,0,
    0,0,20,0,0,0,
    0,0,84,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    17,0,0,0,3,0,
    0,0,85,0,0,0,
    0,0,86,0,0,0,
    11,0,0,0,2,0,
    0,0,85,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,7,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,1,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    5,0,0,0,0,0,
    2,2,0,0,0,0,
    0,0,39,0,0,0,
    0,0,59,0,0,0,
    10,0,0,0,11,0,
    6,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,5,0,
    0,0,2,0,0,0,
    0,0,8,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    19,2,1,0,20,0,
    0,0,2,0,0,0,
    0,0,87,0,0,0,
    0,0,42,0,0,0,
    10,3,0,0,7,0,
    3,0,0,0,0,0,
    0,0,46,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,4,0,0,7,0,
    3,0,0,0,0,0,
    0,0,8,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,4,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,4,0,
    0,0,2,0,0,0,
    0,0,87,0,0,0,
    0,0,88,0,0,0,
    12,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,40,0,0,0,
    5,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    10,0,0,0,14,0,
    17,1,0,0,3,0,
    0,0,85,0,0,0,
    0,0,86,0,0,0,
    11,0,0,0,2,0,
    0,0,85,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    5,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,41,0,0,0,
    0,0,26,0,0,0,
    0,0,2,0,0,0,
    2,2,0,0,0,0,
    19,3,2,0,6,0,
    0,0,2,0,0,0,
    0,0,89,0,0,0,
    0,0,5,0,0,0,
    0,0,2,0,0,0,
    1,4,0,0,0,0,
    11,0,0,0,6,0,
    19,3,1,0,4,0,
    0,0,2,0,0,0,
    0,0,20,0,0,0,
    0,0,89,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    10,5,0,0,7,0,
    3,0,0,0,0,0,
    0,0,8,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,5,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,8,0,0,9,0,
    3,0,0,0,0,0,
    0,0,90,0,0,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,8,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,6,0,0,8,0,
    3,0,0,0,0,0,
    0,0,4,0,0,0,
    0,0,5,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,6,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,7,0,0,5,0,
    3,0,0,0,0,0,
    0,0,18,0,0,0,
    0,0,2,0,0,0,
    2,7,0,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    20,0,0,0,2,0,
    1,0,0,0,0,0,
    11,0,0,0,12,0,
    10,1,0,0,10,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,1,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,91,0,0,0,
    0,0,2,0,0,0,
    24,2,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    10,0,0,0,14,0,
    17,1,0,0,3,0,
    0,0,85,0,0,0,
    0,0,86,0,0,0,
    11,0,0,0,2,0,
    0,0,85,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    5,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,42,0,0,0,
    19,2,1,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,24,0,
    19,2,2,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,18,0,
    19,2,3,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,12,0,
    19,2,4,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,6,0,
    19,2,5,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    19,4,2,0,6,0,
    0,0,2,0,0,0,
    0,0,89,0,0,0,
    0,0,5,0,0,0,
    0,0,2,0,0,0,
    1,5,0,0,0,0,
    11,0,0,0,6,0,
    19,4,1,0,4,0,
    0,0,2,0,0,0,
    0,0,20,0,0,0,
    0,0,89,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    10,6,0,0,7,0,
    3,0,0,0,0,0,
    0,0,46,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,6,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,7,0,0,5,0,
    3,0,0,0,0,0,
    0,0,26,0,0,0,
    0,0,2,0,0,0,
    2,7,0,0,0,0,
    12,0,0,0,0,0,
    10,8,0,0,7,0,
    3,0,0,0,0,0,
    0,0,8,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,8,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,11,0,0,9,0,
    3,0,0,0,0,0,
    0,0,90,0,0,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,11,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,9,0,0,8,0,
    3,0,0,0,0,0,
    0,0,4,0,0,0,
    0,0,5,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,9,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,10,0,0,5,0,
    3,0,0,0,0,0,
    0,0,18,0,0,0,
    0,0,2,0,0,0,
    2,10,0,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    6,0,0,0,0,0,
    10,0,0,0,14,0,
    17,1,0,0,3,0,
    0,0,85,0,0,0,
    0,0,86,0,0,0,
    11,0,0,0,2,0,
    0,0,85,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    5,0,0,0,0,0,
    12,0,0,0,0,0,
    19,2,1,0,2,0,
    0,0,64,0,0,0,
    11,0,0,0,31,0,
    0,0,43,0,0,0,
    19,3,1,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,60,0,0,0,
    11,0,0,0,24,0,
    19,3,2,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,61,0,0,0,
    11,0,0,0,18,0,
    19,3,3,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,62,0,0,0,
    11,0,0,0,12,0,
    19,3,4,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,63,0,0,0,
    11,0,0,0,6,0,
    19,3,5,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,93,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    10,5,0,0,10,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,5,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,6,0,0,3,0,
    3,0,0,0,0,0,
    2,6,0,0,0,0,
    11,0,0,0,4,0,
    0,0,2,0,0,0,
    0,0,47,0,0,0,
    0,0,94,0,0,0,
    12,0,0,0,0,0,
    10,7,0,0,3,0,
    3,0,0,0,0,0,
    2,7,0,0,0,0,
    12,0,0,0,0,0,
    10,8,0,0,9,0,
    3,0,0,0,0,0,
    0,0,90,0,0,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,8,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    19,0,11,0,6,0,
    23,1,0,0,0,0,
    3,0,0,0,0,0,
    0,0,28,0,0,0,
    0,0,2,0,0,0,
    23,2,0,0,1,0,
    11,0,0,0,16,0,
    19,0,12,0,6,0,
    23,1,0,0,0,0,
    3,0,0,0,0,0,
    0,0,92,0,0,0,
    0,0,2,0,0,0,
    23,2,0,0,1,0,
    11,0,0,0,8,0,
    6,0,0,0,0,0,
    23,1,0,0,0,0,
    3,0,0,0,0,0,
    21,0,4,0,0,0,
    0,0,2,0,0,0,
    23,2,0,0,1,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    19,0,3,0,4,0,
    0,0,20,0,0,0,
    0,0,2,0,0,0,
    23,1,22,0,0,0,
    11,0,0,0,15,0,
    19,0,0,0,3,0,
    0,0,96,0,0,0,
    23,1,22,0,0,0,
    11,0,0,0,10,0,
    19,0,1,0,3,0,
    0,0,95,0,0,0,
    23,1,22,0,0,0,
    11,0,0,0,5,0,
    19,0,2,0,3,0,
    0,0,114,0,0,0,
    23,1,22,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    1,1,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    2,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    1,0,0,0,0,0,
    20,0,0,0,2,0,
    1,0,0,0,0,0,
    11,0,0,0,2,0,
    0,0,115,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    1,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    18,1,1,0,3,0,
    0,0,1,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    18,1,4,0,3,0,
    0,0,3,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    18,1,2,0,2,0,
    0,0,97,0,0,0,
    11,0,0,0,7,0,
    10,2,0,0,5,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    10,3,0,0,15,0,
    6,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,7,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,8,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,4,0,0,5,0,
    0,0,2,0,0,0,
    0,0,9,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    25,0,0,9,0,0,
    0,0,2,0,0,0,
    0,0,68,0,0,0,
    0,0,2,0,0,0,
    1,1,0,0,0,0,
    0,0,116,0,0,0,
    0,0,0,0,0,0,
    19,0,0,0,2,0,
    0,0,63,0,0,0,
    11,0,0,0,12,0,
    19,0,1,0,2,0,
    0,0,60,0,0,0,
    11,0,0,0,8,0,
    19,0,2,0,2,0,
    0,0,61,0,0,0,
    11,0,0,0,4,0,
    19,0,3,0,2,0,
    0,0,62,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,4,0,
    0,0,74,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    0,0,6,0,0,0,
    10,1,0,0,3,0,
    2,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    2,0,0,0,0,0,
    0,0,117,0,0,0,
    0,0,2,0,0,0,
    21,0,26,0,0,0,
    17,1,0,0,4,0,
    0,0,2,0,0,0,
    0,0,80,0,0,0,
    0,0,19,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,2,0,0,0,0,
    0,0,121,0,0,0,
    0,0,79,0,0,0,
    0,0,2,0,0,0,
    10,2,0,0,3,0,
    2,2,0,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    19,0,0,0,5,0,
    0,0,122,0,0,0,
    0,0,123,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    11,0,0,0,34,0,
    19,0,1,0,12,0,
    0,0,122,0,0,0,
    17,1,0,0,3,0,
    0,0,2,0,0,0,
    0,0,124,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    0,0,2,0,0,0,
    0,0,123,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    11,0,0,0,20,0,
    19,0,2,0,8,0,
    0,0,117,0,0,0,
    17,1,0,0,3,0,
    0,0,2,0,0,0,
    0,0,124,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    11,0,0,0,10,0,
    19,0,3,0,8,0,
    0,0,125,0,0,0,
    17,1,0,0,3,0,
    0,0,2,0,0,0,
    0,0,124,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,5,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    19,0,0,0,14,0,
    0,0,126,0,0,0,
    19,1,1,0,3,0,
    0,0,2,0,0,0,
    0,0,53,0,0,0,
    12,0,0,0,0,0,
    19,1,2,0,3,0,
    0,0,2,0,0,0,
    0,0,54,0,0,0,
    12,0,0,0,0,0,
    19,1,3,0,3,0,
    0,0,2,0,0,0,
    0,0,127,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,12,0,
    19,0,1,0,2,0,
    0,0,128,0,0,0,
    11,0,0,0,8,0,
    19,0,2,0,2,0,
    0,0,60,0,0,0,
    11,0,0,0,4,0,
    19,0,3,0,2,0,
    0,0,36,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    17,2,0,0,3,0,
    0,0,2,0,0,0,
    0,0,129,0,0,0,
    12,0,0,0,0,0,
    20,3,0,0,3,0,
    0,0,2,0,0,0,
    1,3,0,0,0,0,
    12,0,0,0,0,0,
    19,0,0,0,4,0,
    0,0,130,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    11,0,0,0,30,0,
    19,0,1,0,8,0,
    0,0,131,0,0,0,
    17,2,0,0,3,0,
    0,0,2,0,0,0,
    0,0,130,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    11,0,0,0,20,0,
    19,0,2,0,18,0,
    0,0,60,0,0,0,
    17,3,0,0,3,0,
    0,0,2,0,0,0,
    0,0,129,0,0,0,
    12,0,0,0,0,0,
    20,4,0,0,3,0,
    0,0,2,0,0,0,
    1,4,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,123,0,0,0,
    17,2,0,0,3,0,
    0,0,2,0,0,0,
    0,0,130,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    18,0,1,0,8,0,
    10,3,0,0,4,0,
    24,3,0,0,0,0,
    0,0,14,0,0,0,
    0,0,97,0,0,0,
    11,0,0,0,2,0,
    0,0,97,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,2,0,
    24,3,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,10,0,
    17,2,0,0,5,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    11,0,0,0,3,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    18,0,1,0,3,0,
    0,0,132,0,0,0,
    0,0,1,0,0,0,
    11,0,0,0,7,0,
    18,0,4,0,3,0,
    0,0,132,0,0,0,
    0,0,3,0,0,0,
    11,0,0,0,2,0,
    0,0,132,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,7,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,1,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,2,0,0,5,0,
    3,0,0,0,0,0,
    0,0,26,0,0,0,
    0,0,2,0,0,0,
    2,2,0,0,0,0,
    12,0,0,0,0,0,
    10,3,0,0,7,0,
    3,0,0,0,0,0,
    0,0,8,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,4,0,0,8,0,
    3,0,0,0,0,0,
    0,0,11,0,0,0,
    0,0,5,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,4,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,5,0,0,7,0,
    3,0,0,0,0,0,
    0,0,133,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,5,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,8,0,0,7,0,
    3,0,0,0,0,0,
    0,0,134,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,8,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,6,0,0,8,0,
    3,0,0,0,0,0,
    0,0,4,0,0,0,
    0,0,5,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,6,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,7,0,0,5,0,
    3,0,0,0,0,0,
    0,0,18,0,0,0,
    0,0,2,0,0,0,
    2,7,0,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    24,0,0,0,0,0,
    19,1,1,0,3,0,
    0,0,2,0,0,0,
    0,0,57,0,0,0,
    12,0,0,0,0,0,
    19,1,2,0,3,0,
    0,0,2,0,0,0,
    0,0,58,0,0,0,
    12,0,0,0,0,0,
    19,2,1,0,4,0,
    0,0,2,0,0,0,
    0,0,135,0,0,0,
    0,0,136,0,0,0,
    12,0,0,0,0,0,
    19,2,2,0,4,0,
    0,0,2,0,0,0,
    0,0,135,0,0,0,
    0,0,137,0,0,0,
    12,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    17,2,0,0,5,0,
    2,1,0,0,0,0,
    0,0,74,0,0,0,
    0,0,2,0,0,0,
    2,0,0,0,0,0,
    11,0,0,0,8,0,
    2,0,0,0,0,0,
    10,1,0,0,5,0,
    0,0,2,0,0,0,
    0,0,138,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    21,0,30,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,40,0,0,0,
    0,0,2,0,0,0,
    16,0,0,0,0,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    10,5,0,0,10,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,5,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    11,0,0,0,5,0,
    17,2,0,0,3,0,
    0,0,0,0,0,0,
    0,0,6,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    10,3,0,0,10,0,
    17,4,0,0,5,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    11,0,0,0,3,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    19,6,2,0,6,0,
    0,0,2,0,0,0,
    0,0,89,0,0,0,
    0,0,5,0,0,0,
    0,0,2,0,0,0,
    1,7,0,0,0,0,
    11,0,0,0,6,0,
    19,6,1,0,4,0,
    0,0,2,0,0,0,
    0,0,20,0,0,0,
    0,0,89,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    10,1,0,0,10,0,
    17,2,0,0,5,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    11,0,0,0,3,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    10,1,0,0,10,0,
    17,2,0,0,5,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    11,0,0,0,3,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    19,0,0,0,25,0,
    2,2,0,0,0,0,
    0,0,74,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    10,4,0,0,5,0,
    5,0,0,0,0,0,
    0,0,39,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    12,0,0,0,0,0,
    10,5,0,0,13,0,
    6,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,146,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,5,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    11,0,0,0,34,0,
    2,2,0,0,0,0,
    5,0,0,0,0,0,
    6,0,0,0,0,0,
    10,1,0,0,3,0,
    2,1,0,0,0,0,
    0,0,2,0,0,0,
    12,0,0,0,0,0,
    0,0,147,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    10,4,0,0,7,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    0,0,39,0,0,0,
    0,0,2,0,0,0,
    2,4,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,5,0,0,13,0,
    6,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,146,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,5,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    2,0,0,0,0,0,
    19,0,0,0,2,0,
    0,0,41,0,0,0,
    11,0,0,0,16,0,
    19,0,1,0,2,0,
    0,0,43,0,0,0,
    11,0,0,0,12,0,
    19,0,2,0,10,0,
    0,0,42,0,0,0,
    10,1,0,0,7,0,
    6,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,148,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    0,0,149,0,0,0,
    15,0,40,0,0,0,
    5,0,0,0,0,0,
    16,0,0,0,0,0,
    0,0,76,0,0,0,
    19,2,1,0,3,0,
    0,0,2,0,0,0,
    0,0,77,0,0,0,
    11,0,0,0,5,0,
    19,2,2,0,3,0,
    0,0,2,0,0,0,
    0,0,78,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,120,0,0,0,
    17,3,0,0,5,0,
    0,0,2,0,0,0,
    0,0,80,0,0,0,
    0,0,20,0,0,0,
    0,0,19,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    0,0,2,0,0,0,
    19,4,0,0,1,0,
    11,0,0,0,16,0,
    19,4,1,0,3,0,
    0,0,2,0,0,0,
    0,0,150,0,0,0,
    11,0,0,0,11,0,
    19,4,2,0,3,0,
    0,0,2,0,0,0,
    0,0,151,0,0,0,
    11,0,0,0,6,0,
    19,4,3,0,4,0,
    0,0,2,0,0,0,
    0,0,152,0,0,0,
    0,0,148,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,6,0,0,0,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,2,0,0,0,
    2,7,0,0,0,0,
    17,5,0,0,5,0,
    0,0,2,0,0,0,
    0,0,153,0,0,0,
    0,0,154,0,0,0,
    0,0,155,0,0,0,
    12,0,0,0,0,0,
    10,8,0,0,5,0,
    5,0,0,0,0,0,
    0,0,37,0,0,0,
    0,0,2,0,0,0,
    2,8,0,0,0,0,
    12,0,0,0,0,0,
    5,0,0,0,0,0,
    0,0,126,0,0,0,
    10,9,0,0,5,0,
    8,0,0,0,0,0,
    5,0,0,0,0,0,
    2,9,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    5,0,0,0,0,0,
    0,0,36,0,0,0,
    0,0,76,0,0,0,
    0,0,71,0,0,0,
    0,0,79,0,0,0,
    17,3,0,0,5,0,
    0,0,2,0,0,0,
    0,0,80,0,0,0,
    0,0,20,0,0,0,
    0,0,19,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,146,0,0,0,
    0,0,2,0,0,0,
    1,2,0,0,0,0,
    17,4,0,0,4,0,
    0,0,0,0,0,0,
    1,5,0,0,0,0,
    0,0,6,0,0,0,
    12,0,0,0,0,0,
    0,0,156,0,0,0,
    0,0,2,0,0,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    19,3,1,0,5,0,
    0,0,2,0,0,0,
    0,0,91,0,0,0,
    0,0,2,0,0,0,
    1,2,0,0,0,0,
    11,0,0,0,6,0,
    19,3,2,0,4,0,
    0,0,0,0,0,0,
    1,2,0,0,0,0,
    0,0,6,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    19,2,1,0,2,0,
    0,0,157,0,0,0,
    11,0,0,0,2,0,
    0,0,158,0,0,0,
    12,0,0,0,0,0,
    20,1,0,0,5,0,
    0,0,2,0,0,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    1,0,0,0,0,0,
    11,0,0,0,5,0,
    20,0,0,0,3,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,159,0,0,0,
    17,0,0,0,3,0,
    0,0,2,0,0,0,
    0,0,160,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    2,2,0,0,0,0,
    10,3,0,0,5,0,
    0,0,2,0,0,0,
    0,0,56,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    12,0,0,0,0,0,
    0,0,161,0,0,0,
    17,0,0,0,3,0,
    0,0,2,0,0,0,
    0,0,160,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    0,0,162,0,0,0,
    20,0,0,0,3,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,5,0,
    0,0,2,0,0,0,
    0,0,93,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    19,0,1,0,4,0,
    0,0,163,0,0,0,
    0,0,164,0,0,0,
    0,0,165,0,0,0,
    11,0,0,0,2,0,
    0,0,163,0,0,0,
    12,0,0,0,0,0,
    5,0,0,0,0,0,
    2,1,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    17,3,0,0,3,0,
    0,0,2,0,0,0,
    0,0,66,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,118,0,0,0,
    17,4,0,0,5,0,
    0,0,2,0,0,0,
    0,0,80,0,0,0,
    0,0,20,0,0,0,
    0,0,19,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,2,0,0,0,
    1,2,0,0,0,0,
    6,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,5,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    10,6,0,0,7,0,
    3,0,0,0,0,0,
    0,0,8,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,6,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    19,2,1,0,3,0,
    0,0,2,0,0,0,
    0,0,77,0,0,0,
    11,0,0,0,5,0,
    19,2,2,0,3,0,
    0,0,2,0,0,0,
    0,0,78,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,119,0,0,0,
    17,3,0,0,5,0,
    0,0,2,0,0,0,
    0,0,80,0,0,0,
    0,0,20,0,0,0,
    0,0,19,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    10,4,0,0,8,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,4,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    12,0,0,0,0,0,
    3,0,0,0,0,0,
    0,0,13,0,0,0,
    7,0,0,0,0,0,
    5,0,0,0,0,0,
    2,5,0,0,0,0,
    22,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    14,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,94,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    19,0,0,0,3,0,
    0,0,166,0,0,0,
    0,0,167,0,0,0,
    11,0,0,0,22,0,
    19,0,1,0,4,0,
    24,1,0,0,0,0,
    0,0,2,0,0,0,
    0,0,167,0,0,0,
    11,0,0,0,16,0,
    19,0,2,0,3,0,
    0,0,168,0,0,0,
    0,0,155,0,0,0,
    11,0,0,0,11,0,
    19,0,3,0,4,0,
    24,1,0,0,0,0,
    0,0,2,0,0,0,
    0,0,169,0,0,0,
    11,0,0,0,5,0,
    19,0,4,0,3,0,
    0,0,166,0,0,0,
    0,0,169,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    19,0,1,0,2,0,
    0,0,170,0,0,0,
    11,0,0,0,8,0,
    19,0,2,0,2,0,
    0,0,171,0,0,0,
    11,0,0,0,4,0,
    19,0,3,0,2,0,
    0,0,172,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    10,3,0,0,8,0,
    0,0,27,0,0,0,
    0,0,2,0,0,0,
    2,2,0,0,0,0,
    0,0,2,0,0,0,
    0,0,28,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    11,0,0,0,2,0,
    2,2,0,0,0,0,
    12,0,0,0,0,0,
    19,1,1,0,5,0,
    0,0,2,0,0,0,
    0,0,173,0,0,0,
    0,0,44,0,0,0,
    0,0,174,0,0,0,
    11,0,0,0,19,0,
    19,1,2,0,5,0,
    0,0,2,0,0,0,
    0,0,173,0,0,0,
    0,0,168,0,0,0,
    0,0,155,0,0,0,
    11,0,0,0,12,0,
    19,1,3,0,4,0,
    0,0,2,0,0,0,
    0,0,173,0,0,0,
    0,0,11,0,0,0,
    11,0,0,0,6,0,
    19,1,4,0,4,0,
    0,0,2,0,0,0,
    0,0,173,0,0,0,
    0,0,175,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    20,0,0,0,2,0,
    1,0,0,0,0,0,
    11,0,0,0,54,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    20,1,0,0,2,0,
    1,1,0,0,0,0,
    12,0,0,0,0,0,
    10,2,0,0,10,0,
    20,1,0,0,2,0,
    3,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,176,0,0,0,
    0,0,5,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    12,0,0,0,0,0,
    10,3,0,0,16,0,
    10,2,0,0,2,0,
    3,0,0,0,0,0,
    11,0,0,0,4,0,
    20,1,0,0,2,0,
    3,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,4,0,0,0,
    0,0,5,0,0,0,
    6,0,0,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,4,0,0,13,0,
    10,2,0,0,2,0,
    3,0,0,0,0,0,
    11,0,0,0,8,0,
    10,3,0,0,2,0,
    3,0,0,0,0,0,
    11,0,0,0,4,0,
    20,1,0,0,2,0,
    3,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    2,4,0,0,0,0,
    12,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    1,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    10,0,0,0,14,0,
    6,0,0,0,0,0,
    0,0,7,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,8,0,0,0,
    0,0,2,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,5,0,
    0,0,2,0,0,0,
    0,0,9,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    20,2,0,0,5,0,
    0,0,2,0,0,0,
    0,0,9,0,0,0,
    0,0,2,0,0,0,
    1,2,0,0,0,0,
    12,0,0,0,0,0,
    2,0,0,0,0,0,
    0,0,2,0,0,0,
    1,1,0,0,0,0,
    17,2,0,0,2,0,
    0,0,177,0,0,0,
    12,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    1,0,0,0,0,0,
    0,0,2,0,0,0,
    1,1,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    1,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    19,0,0,0,2,0,
    1,1,0,0,0,0,
    11,0,0,0,12,0,
    19,0,1,0,10,0,
    6,0,0,0,0,0,
    0,0,178,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    17,1,0,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,179,0,0,0,
    0,0,79,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    10,2,0,0,5,0,
    0,0,2,0,0,0,
    0,0,146,0,0,0,
    0,0,2,0,0,0,
    2,2,0,0,0,0,
    12,0,0,0,0,0,
    10,3,0,0,8,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    12,0,0,0,0,0,
    10,4,0,0,3,0,
    3,0,0,0,0,0,
    0,0,13,0,0,0,
    12,0,0,0,0,0,
    10,6,0,0,3,0,
    3,0,0,0,0,0,
    0,0,13,0,0,0,
    12,0,0,0,0,0,
    7,0,0,0,0,0,
    10,4,0,0,3,0,
    5,0,0,0,0,0,
    2,4,0,0,0,0,
    12,0,0,0,0,0,
    10,6,0,0,3,0,
    5,0,0,0,0,0,
    2,6,0,0,0,0,
    12,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    17,1,0,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,179,0,0,0,
    0,0,119,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    10,2,0,0,8,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    12,0,0,0,0,0,
    3,0,0,0,0,0,
    0,0,13,0,0,0,
    7,0,0,0,0,0,
    5,0,0,0,0,0,
    2,3,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    17,1,0,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,179,0,0,0,
    0,0,180,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    0,0,0,0,0,0,
    10,2,0,0,6,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    5,0,0,0,0,0,
    0,0,181,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    5,0,0,0,0,0,
    0,0,13,0,0,0,
    5,0,0,0,0,0,
    2,4,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    17,1,0,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,179,0,0,0,
    0,0,180,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    0,0,0,0,0,0,
    10,2,0,0,6,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    5,0,0,0,0,0,
    0,0,181,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    5,0,0,0,0,0,
    0,0,182,0,0,0,
    0,0,123,0,0,0,
    0,0,2,0,0,0,
    1,4,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    17,1,0,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,179,0,0,0,
    0,0,118,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,2,0,0,0,
    1,2,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,3,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,76,0,0,0,
    17,1,0,0,4,0,
    0,0,2,0,0,0,
    0,0,92,0,0,0,
    0,0,64,0,0,0,
    12,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,179,0,0,0,
    0,0,183,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    0,0,0,0,0,0,
    10,4,0,0,6,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,4,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    12,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    5,0,0,0,0,0,
    0,0,181,0,0,0,
    0,0,2,0,0,0,
    1,2,0,0,0,0,
    5,0,0,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    1,3,0,0,0,0,
    0,0,184,0,0,0,
    0,0,179,0,0,0,
    0,0,185,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    0,0,117,0,0,0,
    0,0,179,0,0,0,
    0,0,118,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,39,0,0,0,
    0,0,2,0,0,0,
    1,1,0,0,0,0,
    0,0,179,0,0,0,
    0,0,156,0,0,0,
    0,0,2,0,0,0,
    1,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,91,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    10,2,0,0,10,0,
    6,0,0,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,2,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    11,0,0,0,6,0,
    20,1,0,0,3,0,
    1,1,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,0,0,0,0,0,
    12,0,0,0,0,0,
    10,3,0,0,10,0,
    17,4,0,0,5,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    11,0,0,0,3,0,
    0,0,2,0,0,0,
    2,3,0,0,0,0,
    12,0,0,0,0,0,
    12,0,0,0,0,0,
    2,0,0,0,0,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    1,1,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,186,0,0,0,
    0,0,187,0,0,0,
    0,0,2,0,0,0,
    21,0,37,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,1,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    20,0,0,0,3,0,
    1,0,0,0,0,0,
    0,0,14,0,0,0,
    12,0,0,0,0,0,
    1,1,0,0,0,0,
    20,2,0,0,5,0,
    0,0,2,0,0,0,
    0,0,13,0,0,0,
    0,0,2,0,0,0,
    1,2,0,0,0,0,
    12,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,132,0,0,0,
    8,0,0,0,0,0,
    3,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    7,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,40,0,0,0,
    5,0,0,0,0,0,
    0,0,190,0,0,0,
    0,0,2,0,0,0,
    16,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    1,0,0,0,0,0,
    22,0,0,0,0,0,
    14,0,0,0,0,0,
    15,0,74,0,0,0,
    3,0,0,0,0,0,
    16,0,0,0,0,0,
    6,0,0,0,0,0,
    0,0,191,0,0,0,
    0,0,192,0,0,0,
    0,0,148,0,0,0,
    0,0,2,0,0,0,
    0,0,0,0,0,0,
    8,0,0,0,0,0,
    4,0,0,0,0,0,
    2,0,0,0,0,0,
    9,0,0,0,0,0,
    4,0,0,0,0,0,
    0,0,6,0,0,0,
    7,0,0,0,0,0,
    10,1,0,0,5,0,
    0,0,2,0,0,0,
    0,0,193,0,0,0,
    0,0,2,0,0,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    10,0,0,0,4,0,
    0,0,26,0,0,0,
    0,0,2,0,0,0,
    2,0,0,0,0,0,
    12,0,0,0,0,0,
    10,1,0,0,2,0,
    2,1,0,0,0,0,
    12,0,0,0,0,0,
    10,2,0,0,5,0,
    5,0,0,0,0,0,
    0,0,190,0,0,0,
    0,0,2,0,0,0,
    2,2,0,0,0,0,
    12,0,0,0,0,0,
};

static const uint32_t perfetto_fmt_ops_count = 16410;

static const uint32_t perfetto_fmt_dispatch[] = {
    0xffff0000,0x00000031,0x0031003a,0x006b0008,0x00730009,0x007c0025,0x00a10009,0x00aa000b,
    0x00b50018,0x00cd0038,0x01050010,0x0115001f,0x01340014,0x01480008,0x01500003,0x01530020,
    0x01730005,0x0178002d,0x01a500ff,0x02a40003,0x02a70005,0x02ac000d,0x02b90005,0x02be009d,
    0x035b0007,0x03620005,0x0367003c,0x03a30021,0x03c40005,0x03c90010,0x03d9002d,0x04060005,
    0x040b0043,0x044e0013,0x04610005,0x0466006d,0x04d30058,0x052b0017,0x05420014,0x05560001,
    0x05570005,0x055c0001,0x055d0005,0x05620009,0x056b0005,0x0570002e,0x059e0001,0x059f0005,
    0x05a40017,0x05bb0005,0x05c0000a,0x05ca002f,0x05f90023,0x061c0023,0x063f0016,0x06550005,
    0x065a004b,0x06a50013,0x06b80005,0x06bd000e,0x06cb0001,0x06cc0005,0x06d1002d,0x06fe0014,
    0x07120014,0x0726003c,0x07620001,0x07630013,0x07760006,0x077c0047,0x07c30018,0x07db0013,
    0x07ee0010,0x07fe0011,0x080f0007,0x0816000b,0x08210009,0x082a002b,0x08550027,0x087c000d,
    0x08890007,0x0890001a,0x08aa0030,0x08da0039,0x09130005,0x09180005,0x091d0005,0x0922001b,
    0x093d0006,0x09430005,0x09480003,0x094b0005,0x09500001,0x09510005,0x0956000f,0x09650001,
    0x0966002c,0x0992001a,0x09ac001e,0x09ca001f,0x09e90018,0x0a01001e,0x0a1f0005,0x0a240009,
    0x0a2d0008,0x0a35001c,0x0a510005,0x0a560005,0x0a5b000a,0x0a65000b,0x0a700005,0x0a750007,
    0x0a7c0007,0x0a830005,0x0a880001,0x0a890005,0x0a8e0013,0x0aa1000e,
};

static const uint32_t perfetto_fmt_dispatch_count = 118;

static const uint8_t perfetto_fmt_prec_table[] = {
    6,0,6,0,7,0,7,0,7,0,4,0,4,0,4,0,
    4,0,3,0,3,0,2,128,1,0,5,1,5,1,5,1,
    5,1,8,0,8,0,8,0,3,0,3,0,0,127,0,127,
    0,127,255,127,3,0,3,0,3,0,3,0,255,127,9,0,
};

static const uint32_t perfetto_fmt_prec_table_count = 64;

static const uint32_t perfetto_fmt_expr_meta[] = {
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0x00001aff,0x00001bff,0x00001cff,0x00001dff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0x00000000,0x00001600,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0x00001fff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
    0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,0xffffffff,
};

static const uint32_t perfetto_fmt_expr_meta_count = 118;


#endif  /* SYNTAQLITE_PERFETTO_DIALECT_FMT_H */
/* ======== end: csrc/dialect_fmt.h ======== */

/* ======== begin: csrc/dialect_roles.h ======== */
#ifndef SYNTAQLITE_PERFETTO_DIALECT_ROLES_H
#define SYNTAQLITE_PERFETTO_DIALECT_ROLES_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT


#include <stdint.h>

/* Semantic role byte array for the perfetto dialect. */
/* Each entry is 8 bytes: 1 discriminant + up to 7 payload bytes. */

static const uint8_t perfetto_roles_data[] = {
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
    0x07,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x08,0x00,0x01,0x00,0x00,0x00,0x00,0x00,
    0x12,0x01,0x02,0x03,0x04,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x05,0x00,0x01,0x02,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x05,0x07,0x04,0x01,0x00,0x00,
    0x0c,0x00,0x02,0x03,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0d,0x00,0x01,0x02,0x00,0x00,0x00,0x00,
    0x11,0x00,0x01,0x03,0x04,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0f,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0f,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0f,0x01,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x07,0x00,0x02,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x13,0x00,0x02,0x03,0x04,0x05,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x06,0x00,0x01,0x03,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0b,0x02,0x01,0x03,0x04,0x05,0x06,0x07,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x09,0x00,0x00,0x03,0x00,0x00,0x00,0x00,
    0x0a,0x00,0x01,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x0e,0x07,0x08,0x09,0x00,0x00,0x00,0x00,
    0x00,0x00,0xff,0xff,0xff,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x01,0x00,0xff,0x05,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x05,0x00,0x01,0xff,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x03,0x02,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x03,0x04,0xff,0x00,0x00,0x00,
    0x01,0x00,0x02,0x03,0x00,0x00,0x00,0x00,
    0x02,0x00,0x02,0x03,0x04,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x04,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
};

static const uint32_t perfetto_roles_count = 118;

/* Macro definition metadata for the perfetto dialect. */
/* Each entry is 8 bytes: node_tag(u16) + 4 field indices + 2 pad. */

static const uint8_t perfetto_macro_defs_data[] = {
    0x65,0x00,0x00,0x03,0x04,0x00,0x00,0x00,
};

static const uint32_t perfetto_macro_defs_count = 1;


#endif  /* SYNTAQLITE_PERFETTO_DIALECT_ROLES_H */
/* ======== end: csrc/dialect_roles.h ======== */

/* ======== begin: csrc/dialect.c ======== */
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT



extern const char synq_perfetto_zKWText[];
extern const unsigned short int synq_perfetto_aKWOffset[];
extern const unsigned char synq_perfetto_aKWLen[];
extern const unsigned char synq_perfetto_aKWCode[];
extern const unsigned int synq_perfetto_nKeyword;

// ============ perfetto dialect descriptor ============


static const SyntaqliteDialectTemplate PERFETTO_DIALECT = {
    .name = "perfetto",

    // AST metadata
    .node_count = sizeof(ast_meta_node_names) / sizeof(ast_meta_node_names[0]),
    .node_names = ast_meta_node_names,
    .field_meta = ast_meta_field_meta,
    .field_meta_counts = ast_meta_field_meta_counts,
    .list_tags = ast_meta_list_tags,
    .range_meta = ast_meta_range_meta,

    // Parser lifecycle
    .parser_alloc = SynqPerfettoParseAlloc,
    .parser_init = SynqPerfettoParseInit,
    .parser_finalize = SynqPerfettoParseFinalize,
    .parser_free = SynqPerfettoParseFree,
    .parser_feed = SynqPerfettoParse,
#ifndef NDEBUG
    .parser_trace = SynqPerfettoParseTrace,
#endif
    .parser_expected_tokens = SynqPerfettoParseExpectedTokens,
    .parser_completion_context = SynqPerfettoParseCompletionContext,
    .parser_fallback = SynqPerfettoParseFallback,

    // Tokenizer
    .get_token = SynqPerfettoGetToken,

    // Keyword table
    .keyword_text = synq_perfetto_zKWText,
    .keyword_offsets = synq_perfetto_aKWOffset,
    .keyword_lens = synq_perfetto_aKWLen,
    .keyword_codes = synq_perfetto_aKWCode,
    .keyword_count = &synq_perfetto_nKeyword,

    // Token metadata
    .token_categories = token_categories,
    .token_type_count = TOKEN_TYPE_COUNT,
    .macro_style = SYNQ_MACRO_STYLE_RUST,

    .fmt_str_data = perfetto_fmt_string_data,
    .fmt_str_offsets = perfetto_fmt_string_offsets,
    .fmt_str_count = perfetto_fmt_string_count,
    .fmt_enum_display = perfetto_fmt_enum_display,
    .fmt_enum_display_count = perfetto_fmt_enum_display_count,
    .fmt_ops = perfetto_fmt_ops,
    .fmt_ops_count = perfetto_fmt_ops_count,
    .fmt_dispatch = perfetto_fmt_dispatch,
    .fmt_dispatch_count = perfetto_fmt_dispatch_count,
    .fmt_prec_table = perfetto_fmt_prec_table,
    .fmt_prec_table_count = perfetto_fmt_prec_table_count,
    .fmt_expr_meta = perfetto_fmt_expr_meta,
    .fmt_expr_meta_count = perfetto_fmt_expr_meta_count,
    .roles_data = perfetto_roles_data,
    .roles_count = perfetto_roles_count,
    .macro_defs_data = perfetto_macro_defs_data,
    .macro_defs_count = perfetto_macro_defs_count,
};

SYNTAQLITE_API SyntaqliteDialect syntaqlite_perfetto_dialect(void) {
    return (SyntaqliteDialect)SYNQ_DIALECT_DEFAULT(&PERFETTO_DIALECT);
}

SYNTAQLITE_API const SyntaqliteDialectTemplate* syntaqlite_perfetto_dialect_template(void) {
    return &PERFETTO_DIALECT;
}

// Local forward decls of the with_dialect entry points (the public
// parser.h / tokenizer.h may have stripped these in inline-dispatch builds).
SYNTAQLITE_API SyntaqliteParser* syntaqlite_parser_create_with_dialect(
    const SyntaqliteMemMethods* mem, SyntaqliteDialect env);
SYNTAQLITE_API SyntaqliteTokenizer* syntaqlite_tokenizer_create_with_dialect(
    const SyntaqliteMemMethods* mem, SyntaqliteDialect env);

SYNTAQLITE_API SyntaqliteParser* syntaqlite_parser_create_perfetto(
    const SyntaqliteMemMethods* mem) {
    return syntaqlite_parser_create_with_dialect(mem, syntaqlite_perfetto_dialect());
}

SYNTAQLITE_API SyntaqliteTokenizer* syntaqlite_tokenizer_create_perfetto(
    const SyntaqliteMemMethods* mem) {
    return syntaqlite_tokenizer_create_with_dialect(mem, syntaqlite_perfetto_dialect());
}
/* ======== end: csrc/dialect.c ======== */

/* ======== begin: csrc/dialect_dispatch.h ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
#ifndef SYNTAQLITE_INTERNAL_DIALECT_DISPATCH_H
#define SYNTAQLITE_INTERNAL_DIALECT_DISPATCH_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Dispatch macros for parser/tokenizer dialect functions.
//
// In amalgamation builds all C code compiles as one unit, so we can call
// dialect functions directly instead of going through function pointers.
//
// Resolution order:
//   1. The Full amalgamation pre-defines SYNQ_PARSER_ALLOC, etc. inline at
//      the top of the .c so this header's branches are skipped entirely.
//      Define SYNTAQLITE_NO_INLINE_DIALECT_DISPATCH to disable that and
//      fall through to the function-pointer fallback below.
//   2. Otherwise, define SYNTAQLITE_INLINE_DIALECT_DISPATCH to a header
//      path that provides the SYNQ_PARSER_ALLOC, etc. macros for your
//      dialect (used by hand-built consumers without the Full amalgamation).
//   3. Otherwise, fall back to function-pointer dispatch through the
//      dialect template struct.


#if defined(SYNTAQLITE_INLINE_DIALECT_DISPATCH)
#include SYNTAQLITE_INLINE_DIALECT_DISPATCH
#elif !defined(SYNQ_PARSER_ALLOC)
// Default: function pointer dispatch through the dialect template struct.
#define SYNQ_PARSER_ALLOC(d, m, c) (d)->parser_alloc(m, c)
#define SYNQ_PARSER_INIT(d, p, c) (d)->parser_init(p, c)
#define SYNQ_PARSER_FINALIZE(d, p) (d)->parser_finalize(p)
#define SYNQ_PARSER_FREE(d, p, f) (d)->parser_free(p, f)
#define SYNQ_PARSER_FEED(d, p, t, m) (d)->parser_feed(p, t, m)
#define SYNQ_PARSER_TRACE(d, f, s) \
  do {                             \
    if ((d)->parser_trace)         \
      (d)->parser_trace(f, s);     \
  } while (0)
#define SYNQ_GET_TOKEN(env, z, t) (env)->tmpl->get_token(env, z, t)
#endif


#endif  /* SYNTAQLITE_INTERNAL_DIALECT_DISPATCH_H */
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/dialect_dispatch.h ======== */

/* ======== begin: csrc/token_wrapped.h ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
#ifndef SYNTAQLITE_INTERNAL_TOKEN_WRAPPED_H
#define SYNTAQLITE_INTERNAL_TOKEN_WRAPPED_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Version-compatibility wrapper for the SQLite tokenizer.
//
// SynqSqliteGetTokenVersionWrapped reclassifies tokens that were
// introduced in newer SQLite versions, so the parser can target
// an older version of the grammar.  It also reclassifies '!' to
// TK_BANG when the caller indicates macro calls are possible
// (Rust-style dialects or callers with macro_fallback enabled).


#include <stdint.h>


int64_t SynqSqliteGetTokenVersionWrapped(const SyntaqliteDialect* env,
                                         uint32_t macro_fallback,
                                         const unsigned char* z,
                                         uint32_t* tokenType);


#endif  /* SYNTAQLITE_INTERNAL_TOKEN_WRAPPED_H */
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/token_wrapped.h ======== */

/* ======== begin: csrc/tokens.h ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
#ifndef SYNTAQLITE_TOKENS_H
#define SYNTAQLITE_TOKENS_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT



#define SYNTAQLITE_TK_ID 40
#define SYNTAQLITE_TK_MINUS 98
#define SYNTAQLITE_TK_PTR 103
#define SYNTAQLITE_TK_STRING 108
#define SYNTAQLITE_TK_JOIN_KW 109
#define SYNTAQLITE_TK_INTEGER 110
#define SYNTAQLITE_TK_FLOAT 111
#define SYNTAQLITE_TK_SEMI 112
#define SYNTAQLITE_TK_LP 113
#define SYNTAQLITE_TK_RP 115
#define SYNTAQLITE_TK_AS 117
#define SYNTAQLITE_TK_COMMA 118
#define SYNTAQLITE_TK_QNUMBER 152
#define SYNTAQLITE_TK_VARIABLE 153
#define SYNTAQLITE_TK_WINDOW 167
#define SYNTAQLITE_TK_OVER 168
#define SYNTAQLITE_TK_FILTER 169
#define SYNTAQLITE_TK_SPACE 185
#define SYNTAQLITE_TK_COMMENT 186
#define SYNTAQLITE_TK_ILLEGAL 187
#define SYNTAQLITE_TK_BANG 188


#endif  /* SYNTAQLITE_TOKENS_H */
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/tokens.h ======== */

/* ======== begin: csrc/parser_internal.h ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
#ifndef SYNTAQLITE_CSRC_PARSER_INTERNAL_H
#define SYNTAQLITE_CSRC_PARSER_INTERNAL_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Private header shared by parser.c, parser_macros.c, and parser_dump.c.


#include <stdint.h>
#include <stdio.h>


#ifdef __cplusplus
extern "C" {
#endif

// ── Tunables ────────────────────────────────────────────────────────────────

#define SYNQ_MAX_MACRO_DEPTH 16

// Per-token comment index entry.  Each entry records where the owning
// token's leading / trailing comments live in `p->comments`.  Used by
// `syntaqlite_token_{leading,trailing}_comments` for O(1) lookup.
typedef struct SynqTokenComments {
  uint32_t leading_first;  // Index into p->comments; UINT32_MAX if count==0.
  uint32_t leading_count;
  uint32_t trailing_first;  // Index into p->comments; UINT32_MAX if count==0.
  uint32_t trailing_count;
} SynqTokenComments;

#define SYNQ_TOKEN_COMMENTS_EMPTY \
  ((SynqTokenComments){UINT32_MAX, 0, UINT32_MAX, 0})

#if defined(__GNUC__) || defined(__clang__)
#define SYNQ_NOINLINE __attribute__((noinline))
#define SYNQ_PRINTF(fmt_idx, va_idx) \
  __attribute__((format(printf, fmt_idx, va_idx)))
#elif defined(_MSC_VER)
#define SYNQ_NOINLINE __declspec(noinline)
#define SYNQ_PRINTF(fmt_idx, va_idx)
#else
#define SYNQ_NOINLINE
#define SYNQ_PRINTF(fmt_idx, va_idx)
#endif

// ── Macro expansion (compiled out with -DSYNTAQLITE_OMIT_MACROS) ─────────
//
// All macro-related types, struct fields, and helper declarations live
// inside this guard.  When SYNTAQLITE_OMIT_MACROS is defined, the parser
// struct shrinks and parser_macros.c / parser_spans.c compile to empty
// translation units.  Public APIs that reference macro state (span_text,
// traceback, expanded_text, etc.) are stubbed in parser.c.

#ifndef SYNTAQLITE_OMIT_MACROS

// A comma-separated argument extracted from a macro call site.
typedef struct SynqMacroArg {
  uint32_t offset;  // Byte offset in the source buffer.
  uint32_t length;  // Byte length of the argument text.
} SynqMacroArg;

// Resolved arg segment on an expansion layer.  Records where a substituted
// arg landed in the expansion buffer and where the original text lives in
// the parent layer, enabling span resolution to drill through $param
// substitutions back to the caller's authored arg text.
//
// `body_offset` / `body_length` record the `$param` token position in the
// macro's authored body (pre-substitution).  Populated by the
// template-expansion path; zero for the set_result_with_arg_map API where
// the caller did not supply authored-body positions.
typedef struct SynqArgSegment {
  uint32_t body_offset;      // Position of $param token in authored body.
  uint32_t body_length;      // Length of $param token in authored body.
  uint32_t sub_offset;       // Where the substituted arg starts in expansion.
  uint32_t sub_length;       // Length in expansion buffer.
  uint32_t origin_layer_id;  // Layer that owns the arg text.
  uint32_t origin_offset;    // Arg text offset in origin layer.
  uint32_t origin_length;    // Arg text length in origin layer.
} SynqArgSegment;

// Expansion layer record.  `_layer_id` on AST spans indexes directly into
// the parser's layers vector.  Entry 0 is a sentinel for the original
// source (expansion_data = source pointer, parent_layer_id = 0,
// call_offset/call_length = 0, name all NULL).
// Actual expansions start at index 1.
//
// `expansion_data` is owned (allocated via p->mem, freed in reset_stmt /
// destroy) for layers produced by the lookup callback.  For the sentinel
// (layer 0, expansion_data = source pointer) and the incremental-API
// begin_macro layers (expansion_data = NULL), the pointer is NOT freed.
//
// `name` borrows from the source buffer or a parent expansion buffer and
// is NOT freed by the parser.
typedef struct SynqExpansionLayer {
  uint32_t call_offset;        // Byte offset of macro call in parent layer.
  uint32_t call_length;        // Byte length of entire macro call.
  const char* expansion_data;  // Expanded text (NULL for sentinel/fallback).
  uint32_t expansion_len;      // Length of expanded text.

  // Definition provenance.
  const char* name;   // Macro name (borrowed), or NULL.
  uint32_t name_len;  // Length of name.
  uint32_t def_line;  // Macro definition line (1-based, 0=unknown).
  uint32_t def_col;   // Macro definition column (1-based, 0=unknown).

  // Arg segments: sorted by sub_offset, non-overlapping.  Allocated via
  // p->mem; freed in reset_stmt / destroy.  NULL when no $param subs.
  SynqArgSegment* arg_segments;
  uint32_t arg_segment_count;

  // Top-level call-site argument spans, populated on every layer
  // that went through `synq_parser_scan_macro_args` (both registered
  // and fallback paths).  Offsets are in the same coordinate system
  // as `call_offset`: statement-relative for top-level layers,
  // otherwise relative to the parent layer's buffer.  Allocated via
  // p->mem and freed in reset_stmt / destroy.  NULL when `name!()`
  // has zero args or the scan overflowed the stack buffer.
  SynqMacroArg* args;
  uint32_t arg_count;

  // 1 if this layer is a fallback layer (unregistered `name!(args)`
  // kept verbatim as a TK_ID — no expansion buffer, no $param
  // substitutions).  0 for registered macros that expanded into
  // `expansion_data`.
  uint32_t is_fallback;

  // Position of this nested call in the *parent's authored body*,
  // computed by inverting the length shifts from the parent's $param
  // substitutions.  Both fields equal SYNTAQLITE_MACRO_BODY_CALL_ARG_INTERNAL
  // (UINT32_MAX) when the call was tokenized from a substituted arg's
  // text (no clean body position) and consumers should descend through
  // the matching arg segment instead.  Zero for top-level layers
  // (parent_layer_id == 0).
  uint32_t body_call_offset;
  uint32_t body_call_length;

  uint32_t parent_layer_id;  // Layer containing the call (0 = source).
} SynqExpansionLayer;

typedef SYNQ_VEC(SynqExpansionLayer) SynqExpansionLayerVec;

// All macro-related parser state, including layer tree and scratch buffers.
// Factored into a single sub-struct so the parser struct has one guarded
// field: `SynqMacroState macro;`.
typedef struct SynqMacroState {
  // ── Configuration ──────────────────────────────────────────────────────
  uint32_t macro_fallback;  // 1 = unregistered name!(args) becomes TK_ID.

  // ── Callback registration ──────────────────────────────────────────────
  SyntaqliteMacroLookupFn lookup_fn;
  void* lookup_user_data;

  // ── Per-invocation state (set before callback, cleared after) ──────────
  // `pending_layer` indexes into layers for the layer the callback
  // should write into via set_result / expand_and_set_result.
  uint32_t pending_layer;
  const SyntaqliteToken* expansion_args;
  uint32_t expansion_arg_count;

  // ── Scratch buffers (reused across invocations, freed in destroy) ──────
  SYNQ_VEC(uint8_t) expand_buf;  // Template expansion output.
  SYNQ_VEC(uint8_t) body_buf;    // NUL-terminated body staging.

  // ── Nesting depth (0 = not in macro) ───────────────────────────────────
  uint32_t depth;

  // ── Layer tree ─────────────────────────────────────────────────────────
  // Entry 0 is a sentinel representing the original source; actual
  // expansions start at index 1.  `_layer_id` on AST spans indexes
  // directly into this vector.
  SynqExpansionLayerVec layers;

  // ── Scratch buffers for span/text APIs ─────────────────────────────────
  // Scratch for `syntaqlite_parser_traceback`.
  SYNQ_VEC(SyntaqliteTracebackFrame) traceback_buf;
  // Scratch for `syntaqlite_parser_node_expanded_text`.
  SYNQ_VEC(uint8_t) node_expanded_buf;
} SynqMacroState;

#endif  // !SYNTAQLITE_OMIT_MACROS

// ── Parser struct ───────────────────────────────────────────────────────────

struct SyntaqliteParser {
  // ── Core ───────────────────────────────────────────────────────────────
  SyntaqliteMemMethods mem;
  SyntaqliteDialect dialect;
  void* lemon;
  SynqParseCtx ctx;
  const char* source;
  uint32_t source_len;
  uint32_t offset;      // Tokenizer cursor into source.
  uint32_t had_error;   // Sticky error flag for current result.
  char error_msg[256];  // Error message buffer.

  // Current statement's byte range.  Set by parser_next / feed_token on
  // the first byte consumed; stmt_end_offset is finalized at statement
  // completion.  `stmt_start_offset == UINT32_MAX` means no statement
  // has been produced yet; `stmt_source` then equals `p->source`.
  //
  // Every layer-0 offset the parser emits (tokens, comments, node
  // extents, arena TextSpan.offset, macro rewrite call_offset with
  // source parent, error_offset) is measured from `stmt_source`.
  uint32_t stmt_start_offset;
  uint32_t stmt_end_offset;
  const char* stmt_source;

  // ── Parser-only state (only parser.c) ──────────────────────────────────
  uint32_t last_token_type;  // Last non-whitespace token fed to Lemon.
  uint32_t finished;         // 1 after EOF has been sent to Lemon.
  uint32_t had_comment;      // 1 if any comment token was seen this stmt.
  // End offset (in `p->ctx.source` coordinates) of the most recent
  // token recorded into `p->tokens` this statement, or `UINT32_MAX`
  // if none.  Used by `synq_parser_record_comment` to classify a
  // comment as TRAILING (same line as the preceding token, in the
  // same layer buffer) vs LEADING (its own line, before any token, or
  // in a different layer from the last push).
  uint32_t last_pushed_token_ctx_end;
  uint32_t last_pushed_token_layer;  // Layer of that token; UINT32_MAX if none.
  int32_t last_status;               // Last SYNTAQLITE_PARSE_* status returned.
  uint32_t trace;
  uint32_t collect_tokens;
  uint32_t sealed;
  uint32_t pending_reset;  // 1 after feed_token signals completion;
                           // cleared on next feed_token call.
  SYNQ_VEC(SyntaqliteComment) comments;
  SYNQ_VEC(SyntaqliteParserToken) tokens;

  // Per-token comment index, parallel to `tokens` (one entry per
  // shifted terminal).  Populated incrementally by
  // `synq_parser_record_comment` and seeded at token-push in
  // `synq_parser_shift_token` from `pending_orphan_leading`.  Lets
  // `syntaqlite_token_{leading,trailing}_comments` answer in O(1)
  // without a separate build step.  `leading_first` / `trailing_first`
  // are `UINT32_MAX` when the corresponding count is zero.
  SYNQ_VEC(SynqTokenComments) token_comments;

  // Orphan bucket for leading comments whose predicted owner token
  // has not been pushed yet.  On the next token push this gets copied
  // into the new `token_comments` entry and reset to empty.  If the
  // statement ends with this still populated, it's the "statement-
  // trailing with no owner" case and is surfaced via
  // `token_leading_comments(ntokens)`.  Only `leading_*` is ever
  // non-empty: trailing comments always have a previous token.
  SynqTokenComments pending_orphan_leading;

  // ── Macro expansion state (compiled out with SYNTAQLITE_OMIT_MACROS) ───
#ifndef SYNTAQLITE_OMIT_MACROS
  SynqMacroState macro;
#endif
};

// ── Cross-file helpers ──────────────────────────────────────────────────────

// Whitespace-or-comment classifier. Centralizes the skip predicate used by
// every site that needs to skip over insignificant tokens (the high-level
// feed_token path, scan_macro_args, and the macro-expansion loop and its
// ID-BANG lookahead).
static inline int synq_token_is_skip(uint32_t type) {
  return type == SYNTAQLITE_TK_SPACE || type == SYNTAQLITE_TK_COMMENT;
}

// Record a comment span into p->comments. `offset` is the byte offset into
// p->source. The owning token_idx and side are computed from the parser's
// current state (last_layer0_token_end + p->tokens length): TRAILING when
// the previous layer-0 token ends on the same source line as this comment,
// LEADING otherwise (in which case the predicted owner is the next token
// to be pushed). Caller must check p->collect_tokens before calling.
void synq_parser_record_comment(SyntaqliteParser* p,
                                uint32_t offset,
                                uint32_t len);

// Set the parser's last_status and return it (used by both parser.c and
// parser_macros.c as a convenient exit helper).
int32_t synq_parser_set_result_status(SyntaqliteParser* p, int32_t rc);

// Unified token shift — the sole path that terminals take into Lemon.
// Pushes to p->tokens (if collect_tokens is on and text is non-null),
// builds the SynqParseToken with a real token_idx, feeds Lemon, and
// updates per-layer parser bookkeeping.
//
// `layer_offset` is interpreted layer-locally:
//   - layer 0: statement-relative
//   - layer N: buffer-local (offset into the expansion layer)
// `p->ctx.layer_id` must already reflect the token's layer.
//
// Returns 1 if Lemon flagged `stmt_completed`, 0 otherwise.  Layer-N
// callers (macro expansion) handle their own error messages and clear
// `p->ctx.error` themselves — this function sets `p->had_error` but
// leaves `p->ctx.error` intact when layer_id != 0.
int synq_parser_shift_token(SyntaqliteParser* p,
                            uint32_t token_type,
                            const char* text,
                            uint32_t len,
                            uint32_t layer_offset);

#ifndef SYNTAQLITE_OMIT_MACROS

// Scan balanced parens for macro args.  Defined in parser_macros.c.
uint32_t synq_parser_scan_macro_args(SyntaqliteParser* p,
                                     const char* source,
                                     uint32_t source_len,
                                     uint32_t bang_offset,
                                     SynqMacroArg* out_args,
                                     uint32_t max_args,
                                     uint32_t* out_end_offset);

// Expand a macro call via the lookup callback, push the expansion layer,
// feed its tokens, and clean up.  Returns 0 on success, -1 if not a
// macro or on error.  Updates *out_end_offset to the position past ')'.
int synq_parser_expand_and_feed_macro(SyntaqliteParser* p,
                                      const char* buf,
                                      uint32_t buf_len,
                                      uint32_t id_offset,
                                      uint32_t id_len,
                                      uint32_t bang_offset,
                                      uint32_t depth,
                                      uint32_t* out_end_offset);

// Try to expand a Rust-style macro call: ID!(args).  Defined in
// parser_macros.c.
int synq_parser_try_macro_call(SyntaqliteParser* p,
                               uint32_t id_offset,
                               uint32_t id_len,
                               uint32_t bang_offset);

static inline int synq_parser_check_macro_straddle(SyntaqliteParser* p) {
  if (!p->ctx.has_macro_straddle)
    return 0;
  snprintf(p->error_msg, sizeof(p->error_msg),
           "macro expansion straddles node boundary");
  p->had_error = 1;
  return -1;
}

// Initialize macro state vecs (callback/expansion fields zeroed by memset).
void synq_macro_state_init(SynqMacroState* m);

// Free all macro state buffers.
void synq_macro_state_free(SynqMacroState* m, SyntaqliteMemMethods mem);

// Free owned expansion data and arg segments on layers 1..N (skip sentinel).
void synq_layers_free_owned(SynqExpansionLayerVec* layers,
                            SyntaqliteMemMethods mem);

// Push the source sentinel at index 0.
void synq_layers_push_sentinel(SynqExpansionLayerVec* layers,
                               const char* source,
                               uint32_t source_len,
                               SyntaqliteMemMethods mem);

#endif  // !SYNTAQLITE_OMIT_MACROS

#ifdef __cplusplus
}
#endif


#endif  /* SYNTAQLITE_CSRC_PARSER_INTERNAL_H */
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/parser_internal.h ======== */

/* ======== begin: csrc/parser.c ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Core parser: lifecycle, main parse loop, result accessors, incremental
// token-feeding API, configuration, arena accessors.
//
// Macro expansion lives in parser_macros.c.  Span resolution and
// traceback live in parser_spans.c.  Per-node extent tracking hooks
// live in parser_extents.c.  AST dump lives in parser_dump.c.
// Cross-file helpers are declared in csrc/parser_internal.h.

#include <stdio.h>
#include <string.h>



// ---------------------------------------------------------------------------
// Forward declarations of file-local helpers
// ---------------------------------------------------------------------------

static void reset_stmt(SyntaqliteParser* p);
static int32_t stmt_boundary(SyntaqliteParser* p);
static int finish_input(SyntaqliteParser* p);

// Record the first byte consumed for the current statement and sync
// the layer-0 macro sentinel so span walkers resolve layer-0 spans
// against the statement slice.
static void synq_open_statement(SyntaqliteParser* p, uint32_t offset) {
  p->stmt_start_offset = offset;
  p->stmt_source = p->source + offset;
#ifndef SYNTAQLITE_OMIT_MACROS
  if (syntaqlite_vec_len(&p->macro.layers) > 0) {
    SynqExpansionLayer* root = &p->macro.layers.data[0];
    root->expansion_data = p->stmt_source;
    root->expansion_len = p->source_len - offset;
  }
#endif
}

int32_t synq_parser_set_result_status(SyntaqliteParser* p, int32_t rc) {
  p->last_status = rc;
  if (p->stmt_start_offset != UINT32_MAX) {
    p->stmt_end_offset =
        p->offset > p->stmt_start_offset ? p->offset : p->stmt_start_offset;
  }
  return rc;
}

// Local short-hand.
#define set_result_status synq_parser_set_result_status

// ---------------------------------------------------------------------------
// Internal: reusable state-reset helpers
// ---------------------------------------------------------------------------

// Reinitialize the Lemon parser automaton to its initial state.
// Called after real-statement completion (cmdx ::= cmd . reduces with SEMI
// as the LALR(1) lookahead, leaving SEMI shifted but ecmd ::= cmdx SEMI .
// pending).  Reinitializing discards that half-reduced state.
// NOT called for bare semicolons or error-recovery completions — those
// reduce via ecmd ::= SEMI . or ecmd ::= error SEMI . using the *next*
// token as the lookahead, so that token is already consumed by Lemon.
static void lemon_reinit(SyntaqliteParser* p) {
  SYNQ_PARSER_FINALIZE(p->dialect.tmpl, p->lemon);
  SYNQ_PARSER_INIT(p->dialect.tmpl, p->lemon, &p->ctx);
  p->last_token_type = 0;
}

// Reset all per-statement output state: arena, token/comment/macro vectors,
// context flags, and error state.  Called at the *start* of the next
// statement (not at completion) so that callers can read the previous
// statement's results between calls.
static void reset_stmt(SyntaqliteParser* p) {
  synq_parse_ctx_clear(&p->ctx);
  syntaqlite_vec_clear(&p->comments);
  syntaqlite_vec_clear(&p->tokens);
  syntaqlite_vec_clear(&p->token_comments);
  p->pending_orphan_leading = SYNQ_TOKEN_COMMENTS_EMPTY;
#ifndef SYNTAQLITE_OMIT_MACROS
  syntaqlite_vec_clear(&p->macro.traceback_buf);
  syntaqlite_vec_clear(&p->macro.node_expanded_buf);
  synq_layers_free_owned(&p->macro.layers, p->mem);
  syntaqlite_vec_clear(&p->macro.layers);
  if (p->source)
    synq_layers_push_sentinel(&p->macro.layers, p->source, p->source_len,
                              p->mem);
#endif
  p->ctx.layer_id = 0;
  p->ctx.cur_shift_start = 0;
  p->ctx.last_shifted_end = 0;
  p->ctx.root = SYNTAQLITE_NULL_NODE;
  p->ctx.stmt_completed = 0;
  p->ctx.pending_explain_mode = 0;
  p->ctx.error = 0;
  p->ctx.saw_subquery = 0;
  p->ctx.saw_update_delete_limit = 0;
  p->had_comment = 0;
  p->last_pushed_token_ctx_end = UINT32_MAX;
  p->last_pushed_token_layer = UINT32_MAX;
  p->had_error = 0;
  p->error_msg[0] = '\0';
  p->ctx.error_offset = 0xFFFFFFFF;
  p->ctx.error_length = 0;
  p->ctx.tokens = p->collect_tokens ? &p->tokens : NULL;
  p->stmt_start_offset = UINT32_MAX;
  p->stmt_end_offset = 0;
  p->stmt_source = p->source;
}

// Handle a statement boundary after shift_token returns 1.
// Reinitializes Lemon and classifies the completed statement:
//   SYNTAQLITE_PARSE_OK    — successful statement; a bare semicolon is one,
//                            with a null root
//   SYNTAQLITE_PARSE_ERROR — statement with syntax error(s)
static int32_t stmt_boundary(SyntaqliteParser* p) {
  lemon_reinit(p);

#ifndef SYNTAQLITE_OMIT_MACROS
  if (synq_parser_check_macro_straddle(p) < 0)
    return SYNTAQLITE_PARSE_ERROR;
#endif

  if (p->had_error) {
    p->had_error = 0;  // consumed for this result
    return SYNTAQLITE_PARSE_ERROR;
  }
  return SYNTAQLITE_PARSE_OK;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

SYNTAQLITE_API SyntaqliteParser* syntaqlite_parser_create_with_dialect(
    const SyntaqliteMemMethods* mem,
    const SyntaqliteDialect dialect) {
  SyntaqliteMemMethods m = mem ? *mem : SYNTAQLITE_MEM_METHODS_DEFAULT;
  SyntaqliteParser* p = m.xMalloc(sizeof(SyntaqliteParser));
  memset(p, 0, sizeof(*p));
  p->mem = m;
  p->dialect = dialect;
  p->lemon = SYNQ_PARSER_ALLOC(dialect.tmpl, m.xMalloc, &p->ctx);
  synq_parse_ctx_init(&p->ctx, m);
  syntaqlite_vec_init(&p->comments);
  syntaqlite_vec_init(&p->tokens);
  syntaqlite_vec_init(&p->token_comments);
  p->pending_orphan_leading = SYNQ_TOKEN_COMMENTS_EMPTY;
#ifndef SYNTAQLITE_OMIT_MACROS
  synq_macro_state_init(&p->macro);
#endif
  return p;
}

#ifndef SYNTAQLITE_OMIT_SQLITE_API
SYNTAQLITE_API SyntaqliteParser* syntaqlite_parser_create(
    const SyntaqliteMemMethods* mem) {
  SyntaqliteDialect dialect = syntaqlite_sqlite_dialect();
  return syntaqlite_parser_create_with_dialect(mem, dialect);
}
#endif

SYNTAQLITE_API void syntaqlite_parser_reset(SyntaqliteParser* p,
                                            const char* source,
                                            uint32_t len) {
  // Seal the parser on first use — configuration is frozen after this.
  p->sealed = 1;

  lemon_reinit(p);
  reset_stmt(p);

  p->source = source;
  p->source_len = len;
  p->offset = 0;
  p->finished = 0;
  p->pending_reset = 0;
  p->last_status = SYNTAQLITE_PARSE_DONE;
  p->stmt_start_offset = UINT32_MAX;
  p->stmt_end_offset = 0;
  p->stmt_source = source;
#ifndef SYNTAQLITE_OMIT_MACROS
  p->macro.depth = 0;
  syntaqlite_vec_clear(&p->macro.layers);
  synq_layers_push_sentinel(&p->macro.layers, source, len, p->mem);
#endif

  p->ctx.source = source;
  p->ctx.env = &p->dialect;
}

SYNTAQLITE_API void syntaqlite_parser_destroy(SyntaqliteParser* p) {
  if (p) {
    SYNQ_PARSER_FREE(p->dialect.tmpl, p->lemon, p->mem.xFree);
    synq_parse_ctx_free(&p->ctx);
    syntaqlite_vec_free(&p->comments, p->mem);
    syntaqlite_vec_free(&p->token_comments, p->mem);
    syntaqlite_vec_free(&p->tokens, p->mem);
#ifndef SYNTAQLITE_OMIT_MACROS
    synq_macro_state_free(&p->macro, p->mem);
#endif
    p->mem.xFree(p);
  }
}

// ---------------------------------------------------------------------------
// Internal: feed one real token to Lemon.
// Returns: 0 = keep going, 1 = statement completed, -1 = unrecoverable error.
// ---------------------------------------------------------------------------

// Unified token shift: push the token to `p->tokens` (when
// `collect_tokens` is on and `text` is non-null), build the
// `SynqParseToken` with a real `token_idx`, feed Lemon, and maintain
// per-layer parser bookkeeping.  The sole path that terminals take
// into Lemon — called by the main tokenizer loop, by the macro
// expansion loop, by the fallback-macro consolidator, and by the
// low-level incremental feed API.
//
// `text` points to the token bytes in their owning buffer (the source
// for layer 0, an expansion layer's buffer for layer N).  May be NULL
// for synthesized tokens (SEMI/EOF at end-of-input); in that case no
// vec entry is pushed and the shift markers do not advance.
//
// `layer_offset` is the token's offset within its owning buffer:
//   - for layer 0: statement-relative (identical to `text - stmt_source`)
//   - for layer N: position within the expansion buffer
// `p->ctx.layer_id` must already reflect the token's layer.
//
// Returns 1 if Lemon flagged the statement as completed, 0 otherwise.
// Layer-N callers (macro expansion) handle their own error messages
// and clear `p->ctx.error` themselves; for them, this function sets
// `p->had_error` but leaves the error flag intact.
int synq_parser_shift_token(SyntaqliteParser* p,
                            uint32_t token_type,
                            const char* text,
                            uint32_t len,
                            uint32_t layer_offset) {
  uint32_t layer_id = p->ctx.layer_id;
  uint32_t tidx = 0xFFFFFFFF;

  if (p->collect_tokens && text) {
    SyntaqliteParserToken tp = {layer_offset, len, token_type, 0, layer_id};
    syntaqlite_vec_push(&p->tokens, tp, p->mem);
    tidx = syntaqlite_vec_len(&p->tokens) - 1;
    // Pair the new token with its comment-index entry.  Seed from
    // `pending_orphan_leading` so comments whose predicted owner was
    // this about-to-be-pushed token land at the right slot, then
    // reset the orphan bucket.
    syntaqlite_vec_push(&p->token_comments, p->pending_orphan_leading, p->mem);
    p->pending_orphan_leading = SYNQ_TOKEN_COMMENTS_EMPTY;
    // Remember this token's end in `p->ctx.source` coordinates so
    // `synq_parser_record_comment` can decide same-line trailing
    // attachment within whichever buffer it is tokenizing.  For
    // layer 0, `ctx.source == p->source` so the end is source-
    // absolute; for layer N, `ctx.source` is the expansion buffer so
    // the end is layer-local.  `layer_id` alongside lets record_comment
    // reject prev-ends from a different layer than the current one.
    p->last_pushed_token_ctx_end =
        layer_id == 0 ? p->stmt_start_offset + layer_offset + len
                      : layer_offset + len;
    p->last_pushed_token_layer = layer_id;
  }

  // BEFORE-style empty-rule markers (ast_builder.h:117) are documented
  // as valid for layer-0 shifts only.  Publish cur_shift_start before
  // SYNQ_PARSER_FEED so reductions firing inside the feed observe it.
  if (layer_id == 0) {
    p->ctx.cur_shift_start = layer_offset;
  }

  SynqParseToken minor = {
      .z = text,
      .n = len,
      .type = token_type,
      .token_idx = tidx,
      .offset = layer_offset,
      .layer_id = layer_id,
  };
  SYNQ_PARSER_FEED(p->dialect.tmpl, p->lemon, (int)token_type, minor);
  p->last_token_type = token_type;

  // AFTER-style markers: also layer-0.  Advance after feed so reductions
  // inside feed saw the previous terminal's end.
  if (layer_id == 0) {
    p->ctx.last_shifted_end = layer_offset + len;
  }

  if (p->ctx.error) {
    p->had_error = 1;
    if (layer_id == 0) {
      if (p->error_msg[0] == '\0') {
        if (text) {
          p->ctx.error_offset = layer_offset;
          p->ctx.error_length = len;
          snprintf(p->error_msg, sizeof(p->error_msg),
                   "syntax error near '%.*s'", len, text);
        } else {
          snprintf(p->error_msg, sizeof(p->error_msg),
                   "incomplete SQL statement");
        }
      }
      p->ctx.error = 0;  // Lemon is now driving recovery.
    }
    // Layer-N: leave p->ctx.error set; expand_and_feed builds a
    // macro-specific message and clears the flag.
    return 0;
  }

  if (p->ctx.stmt_completed) {
    p->ctx.stmt_completed = 0;
    return 1;
  }

  return 0;
}

// Local shorthand for the cross-file helper.
#define shift_token synq_parser_shift_token

// ---------------------------------------------------------------------------
// Internal: synthesize SEMI + EOF to finish parsing.
// Returns a SYNTAQLITE_PARSE_* code.
// ---------------------------------------------------------------------------

static int finish_input(SyntaqliteParser* p) {
  // No real tokens were fed (only whitespace/comments).
  if (p->last_token_type == 0) {
    p->finished = 1;
    // If comments were seen, return PARSE_OK (root will be NULL_NODE).
    // This matches SQLite's sqlite3_prepare_v2 which returns SQLITE_OK
    // for comment-only input.
    if (p->had_comment) {
      return set_result_status(p, SYNTAQLITE_PARSE_OK);
    }
    return set_result_status(p, SYNTAQLITE_PARSE_DONE);
  }

  // Synthesize SEMI if the last token wasn't one.
  if (p->last_token_type != SYNTAQLITE_TK_SEMI) {
    int rc = shift_token(p, SYNTAQLITE_TK_SEMI, NULL, 0, 0);
    if (rc == 1) {
      p->finished = 1;
      return set_result_status(p, stmt_boundary(p));
    }
  }

  // Send end-of-input (EOF) to flush the final reduction.
  SynqParseToken eof = {.z = NULL,
                        .n = 0,
                        .type = 0,
                        .token_idx = 0xFFFFFFFF,
                        .offset = 0,
                        .layer_id = 0};
  SYNQ_PARSER_FEED(p->dialect.tmpl, p->lemon, 0, eof);
  p->finished = 1;

  if (p->ctx.error) {
    p->had_error = 1;
    if (p->ctx.error_offset == 0xFFFFFFFF) {
      p->ctx.error_offset = p->offset - p->stmt_start_offset;
    }
    if (p->error_msg[0] == '\0') {
      snprintf(p->error_msg, sizeof(p->error_msg), "incomplete SQL statement");
    }
    return set_result_status(p, SYNTAQLITE_PARSE_ERROR);
  }

  if (p->ctx.root != SYNTAQLITE_NULL_NODE) {
#ifndef SYNTAQLITE_OMIT_MACROS
    if (synq_parser_check_macro_straddle(p) < 0)
      return set_result_status(p, SYNTAQLITE_PARSE_ERROR);
#endif
    return set_result_status(
        p, p->had_error ? SYNTAQLITE_PARSE_ERROR : SYNTAQLITE_PARSE_OK);
  }

  if (p->had_error)
    return set_result_status(p, SYNTAQLITE_PARSE_ERROR);

  return set_result_status(p, SYNTAQLITE_PARSE_DONE);
}

// ---------------------------------------------------------------------------
// Internal: token recording and feeding
// ---------------------------------------------------------------------------

// Record a comment token (outlined from the hot loop).
//
// `offset` is interpreted in `p->ctx.source` coordinates:
//   - layer 0: source-absolute (ctx.source == p->source).
//   - layer N: layer-local (ctx.source is the expansion buffer).
//
// Classification:
//   - Same-line with prev token + something else on the line after this
//     comment → LEADING on the next token (block "between two tokens").
//   - Same-line with prev + rest of line empty (whitespace/comments only)
//     → TRAILING on prev token.  Line comments (`-- ...`) always hit this
//     branch because they consume the rest of their line.
//   - Not same-line with prev → LEADING on the next token.
//
// Same-line-with-prev is computed in the prev token's buffer (same-layer
// case) or, for a source comment after a macro expansion, against the
// call's end position in source (cross-layer case).  Same-line-with-
// follower is a forward scan in the comment's own buffer; it sees past
// whitespace and adjacent comments.

// Offset of the next real (non-whitespace, non-comment) token in `buf`
// starting at `pos`, or `buf_len` if the rest of the buffer is
// whitespace and comments only.
static uint32_t synq_next_token_offset(SyntaqliteParser* p,
                                       const char* buf,
                                       uint32_t pos,
                                       uint32_t buf_len) {
  while (pos < buf_len && buf[pos] != '\0') {
    uint32_t tt = 0;
    int64_t tl = SynqSqliteGetTokenVersionWrapped(
        &p->dialect, 0, (const unsigned char*)buf + pos, &tt);
    if (tl <= 0)
      return buf_len;
    if (!synq_token_is_skip(tt))
      return pos;
    pos += (uint32_t)tl;
  }
  return buf_len;
}

SYNQ_NOINLINE
void synq_parser_record_comment(SyntaqliteParser* p,
                                uint32_t offset,
                                uint32_t len) {
  uint32_t layer = p->ctx.layer_id;
  const unsigned char* z = (const unsigned char*)p->ctx.source;
  int is_block = (z[offset] != '-');

  // ── Same-line with previous token? ───────────────────────────────────────
  int same_line_with_prev = 0;
  if (p->last_pushed_token_layer == layer &&
      p->last_pushed_token_ctx_end != UINT32_MAX &&
      p->last_pushed_token_ctx_end <= offset) {
    same_line_with_prev = memchr(z + p->last_pushed_token_ctx_end, '\n',
                                 offset - p->last_pushed_token_ctx_end) == NULL;
  }
#ifndef SYNTAQLITE_OMIT_MACROS
  else if (layer == 0 && p->last_pushed_token_layer != UINT32_MAX &&
           p->last_pushed_token_layer > 0 &&
           p->last_pushed_token_layer < syntaqlite_vec_len(&p->macro.layers)) {
    // Prev token was inside an expansion; comment is in source.  Compare
    // against the end of the topmost ancestor macro call in source.
    uint32_t L = p->last_pushed_token_layer;
    while (L > 0 && p->macro.layers.data[L].parent_layer_id != 0)
      L = p->macro.layers.data[L].parent_layer_id;
    if (L > 0) {
      const SynqExpansionLayer* lyr = &p->macro.layers.data[L];
      uint32_t call_end =
          p->stmt_start_offset + lyr->call_offset + lyr->call_length;
      if (call_end <= offset)
        same_line_with_prev = memchr((const unsigned char*)p->source + call_end,
                                     '\n', offset - call_end) == NULL;
    }
  }
#endif

  // ── Trailing on prev iff same-line-with-prev AND next token (if any)
  //    is on a different line.  Line comments always satisfy the latter
  //    (they consume the rest of their line).
  int is_trailing = 0;
  if (same_line_with_prev) {
    if (!is_block) {
      is_trailing = 1;
    } else {
      uint32_t buf_len = layer == 0 ? p->source_len
#ifndef SYNTAQLITE_OMIT_MACROS
                                    : p->macro.layers.data[layer].expansion_len
#else
                                    : 0
#endif
          ;
      uint32_t next =
          synq_next_token_offset(p, (const char*)z, offset + len, buf_len);
      is_trailing = (next >= buf_len) || memchr(z + offset + len, '\n',
                                                next - (offset + len)) != NULL;
    }
  }

  // ── Record the comment ───────────────────────────────────────────────────
  SyntaqliteComment t = {
      .offset = layer == 0 ? offset - p->stmt_start_offset : offset,
      .length = len,
      .token_idx = 0,  // filled below
      .kind = is_block ? (uint8_t)1 : (uint8_t)0,
      .side = is_trailing ? SYNQ_COMMENT_TRAILING : SYNQ_COMMENT_LEADING,
      .layer_id = (uint8_t)(layer > 0xFF ? 0xFF : layer),
      ._pad = 0,
  };
  uint32_t comment_idx = syntaqlite_vec_len(&p->comments);
  if (is_trailing) {
    uint32_t tok_idx = syntaqlite_vec_len(&p->tokens) - 1;
    t.token_idx = tok_idx;
    SynqTokenComments* tc = &syntaqlite_vec_at(&p->token_comments, tok_idx);
    if (tc->trailing_count == 0)
      tc->trailing_first = comment_idx;
    tc->trailing_count++;
  } else {
    t.token_idx = syntaqlite_vec_len(&p->tokens);  // predicted next token
    SynqTokenComments* tc = &p->pending_orphan_leading;
    if (tc->leading_count == 0)
      tc->leading_first = comment_idx;
    tc->leading_count++;
  }
  syntaqlite_vec_push(&p->comments, t, p->mem);
}

// ---------------------------------------------------------------------------
// High-level API
// ---------------------------------------------------------------------------

// Tokenize the next non-whitespace token, recording any comments along the
// way.  Returns the token length (0 at end-of-input).  `*out_offset` and
// `*out_type` are set to the position and type of the returned token.
static int64_t next_token(SyntaqliteParser* p,
                          const unsigned char* z,
                          uint32_t pos,
                          uint32_t* out_offset,
                          uint32_t* out_type) {
  while (pos < p->source_len && z[pos] != '\0') {
    uint32_t type = 0;
#ifdef SYNTAQLITE_OMIT_MACROS
    int64_t len =
        SynqSqliteGetTokenVersionWrapped(&p->dialect, 0, z + pos, &type);
#else
    int64_t len = SynqSqliteGetTokenVersionWrapped(
        &p->dialect, p->macro.macro_fallback, z + pos, &type);
#endif
    if (len <= 0)
      return 0;
    if (type == SYNTAQLITE_TK_SPACE) {
      pos += (uint32_t)len;
      continue;
    }
    *out_offset = pos;
    *out_type = type;
    return len;
  }
  *out_offset = pos;
  *out_type = 0;
  return 0;
}

// ---------------------------------------------------------------------------
// Context-sensitive keyword analysis (mirrors SQLite's analyze*Keyword).
//
// WINDOW, OVER, and FILTER are context-sensitive keywords in SQLite: they act
// as keywords only in specific syntactic positions and are valid identifiers
// everywhere else.  The Lemon grammar cannot handle this via %fallback because
// that would create ambiguity (e.g. `SELECT sum(x) OVER ...` — OVER could be
// a keyword or an alias).
//
// The logic here mirrors the vendored upstream functions in
// sqlite-vendored/sources/fragments/window_keyword_analysis.c.
// ---------------------------------------------------------------------------

// Peek the next non-whitespace, non-comment token via the dialect tokenizer.
// Normalizes identifier-like tokens to TK_ID (mirrors SQLite's getToken
// helper from sqlite-vendored/sources/fragments/window_keyword_analysis.c).
static int synq_peek_token(SyntaqliteParser* p, const unsigned char** pz) {
  const unsigned char* z = *pz;
  int t;
  do {
    int raw = 0;
    z += SYNQ_GET_TOKEN(&p->dialect, z, &raw);
    t = raw;
  } while (t == SYNTAQLITE_TK_SPACE || t == SYNTAQLITE_TK_COMMENT);
  if (t == SYNTAQLITE_TK_ID || t == SYNTAQLITE_TK_STRING ||
      t == SYNTAQLITE_TK_JOIN_KW || t == SYNTAQLITE_TK_WINDOW ||
      t == SYNTAQLITE_TK_OVER ||
      p->dialect.tmpl->parser_fallback(t) == SYNTAQLITE_TK_ID) {
    t = SYNTAQLITE_TK_ID;
  }
  *pz = z;
  return t;
}

// WINDOW → keyword only when followed by <id> AS (a named window def).
static uint32_t synq_analyze_window(SyntaqliteParser* p,
                                    const unsigned char* z) {
  int t = synq_peek_token(p, &z);
  if (t != SYNTAQLITE_TK_ID)
    return SYNTAQLITE_TK_ID;
  t = synq_peek_token(p, &z);
  if (t != SYNTAQLITE_TK_AS)
    return SYNTAQLITE_TK_ID;
  return SYNTAQLITE_TK_WINDOW;
}

// OVER → keyword only when prev was ')' and next is '(' or <id>.
static uint32_t synq_analyze_over(SyntaqliteParser* p,
                                  const unsigned char* z,
                                  uint32_t last_token_type) {
  if (last_token_type == SYNTAQLITE_TK_RP) {
    int t = synq_peek_token(p, &z);
    if (t == SYNTAQLITE_TK_LP || t == SYNTAQLITE_TK_ID)
      return SYNTAQLITE_TK_OVER;
  }
  return SYNTAQLITE_TK_ID;
}

// FILTER → keyword only when prev was ')' and next is '('.
static uint32_t synq_analyze_filter(SyntaqliteParser* p,
                                    const unsigned char* z,
                                    uint32_t last_token_type) {
  if (last_token_type == SYNTAQLITE_TK_RP) {
    int t = synq_peek_token(p, &z);
    if (t == SYNTAQLITE_TK_LP)
      return SYNTAQLITE_TK_FILTER;
  }
  return SYNTAQLITE_TK_ID;
}

SYNTAQLITE_API int32_t syntaqlite_parser_next(SyntaqliteParser* p) {
  reset_stmt(p);

  if (p->finished)
    return set_result_status(p, SYNTAQLITE_PARSE_DONE);

  const unsigned char* z = (const unsigned char*)p->source;

  // 1-token lookahead: tokenize the first token before entering the loop.
  uint32_t cur_type = 0;
  uint32_t cur_offset = 0;
  int64_t cur_len = next_token(p, z, p->offset, &cur_offset, &cur_type);

  if (cur_len > 0 && p->stmt_start_offset == UINT32_MAX) {
    synq_open_statement(p, cur_offset);
  }

  while (cur_len > 0) {
    // Handle comments: record and advance without feeding to Lemon.
    // This keeps comment recording in the main loop so that lookahead
    // never eagerly consumes comments belonging to the next statement.
    if (cur_type == SYNTAQLITE_TK_COMMENT) {
      p->had_comment = 1;
      if (p->collect_tokens)
        synq_parser_record_comment(p, cur_offset, (uint32_t)cur_len);
      // Advance p->offset so this comment is covered by stmt_end_offset
      // (otherwise text() would not span trailing/comment-only comments).
      p->offset = cur_offset + (uint32_t)cur_len;
      cur_len = next_token(p, z, p->offset, &cur_offset, &cur_type);
      continue;
    }

    p->offset = cur_offset + (uint32_t)cur_len;

    // Context-sensitive keyword reclassification: WINDOW/OVER/FILTER may
    // need to be demoted to TK_ID depending on surrounding tokens.
    if (cur_type == SYNTAQLITE_TK_WINDOW) {
      cur_type = synq_analyze_window(p, z + p->offset);
    } else if (cur_type == SYNTAQLITE_TK_OVER) {
      cur_type = synq_analyze_over(p, z + p->offset, p->last_token_type);
    } else if (cur_type == SYNTAQLITE_TK_FILTER) {
      cur_type = synq_analyze_filter(p, z + p->offset, p->last_token_type);
    }

    // Tokenize the lookahead — always one token ahead.
    uint32_t la_offset = 0;
    uint32_t la_type = 0;
    int64_t la_len = next_token(p, z, p->offset, &la_offset, &la_type);

#ifndef SYNTAQLITE_OMIT_MACROS
    // Macro detection: ID followed by TK_BANG ('!').  The token wrapper
    // produces TK_BANG for any dialect that may have macro calls (Rust-style
    // dialects or any dialect with macro_fallback enabled).
    if (cur_type == SYNTAQLITE_TK_ID && la_type == SYNTAQLITE_TK_BANG) {
      int mrc = synq_parser_try_macro_call(p, cur_offset, (uint32_t)cur_len,
                                           la_offset);
      if (mrc == 1)
        return set_result_status(p, stmt_boundary(p));
      if (mrc == 0) {
        // Macro consumed tokens past the lookahead — re-tokenize.
        cur_len = next_token(p, z, p->offset, &cur_offset, &cur_type);
        continue;
      }
    }
#endif

    // Normal token (or macro fallthrough): shift into Lemon.
    //
    // `ecmd ::= SEMI` (an empty statement) and `ecmd ::= error SEMI`
    // (recovery) both reduce only once the *next* token arrives as the
    // LALR(1) lookahead, by which point Lemon has swallowed the first
    // token of the following statement.  Close the statement on the
    // semicolon instead; lemon_reinit discards the pending reduction.
    int at_stmt_start = p->last_token_type == 0;
    uint32_t main_layer_offset = cur_offset - p->stmt_start_offset;
    int main_rc = shift_token(p, cur_type, p->source + cur_offset,
                              (uint32_t)cur_len, main_layer_offset);
    if (main_rc == 0 && cur_type == SYNTAQLITE_TK_SEMI &&
        (at_stmt_start || p->had_error))
      main_rc = 1;
    if (main_rc == 1) {
      // Eagerly consume same-line trailing comments after the statement
      // terminator so they attach to this statement's last token instead
      // of the next statement's first.  Stop at the first newline or
      // non-skip token; own-line comments belong to the next statement.
      uint32_t scan = p->offset;
      while (scan < p->source_len && z[scan] != '\0') {
        uint32_t tt = 0;
        int64_t tl =
            SynqSqliteGetTokenVersionWrapped(&p->dialect, 0, z + scan, &tt);
        if (tl <= 0)
          break;
        if (tt == SYNTAQLITE_TK_SPACE) {
          if (memchr(z + scan, '\n', (size_t)tl) != NULL)
            break;
          scan += (uint32_t)tl;
          continue;
        }
        if (tt == SYNTAQLITE_TK_COMMENT) {
          if (p->collect_tokens)
            synq_parser_record_comment(p, scan, (uint32_t)tl);
          scan += (uint32_t)tl;
          p->offset = scan;
          continue;
        }
        break;
      }
      return set_result_status(p, stmt_boundary(p));
    }

    // Shift: lookahead becomes current.
    cur_type = la_type;
    cur_offset = la_offset;
    cur_len = la_len;
  }

  // End of input.
  return finish_input(p);
}

// ---------------------------------------------------------------------------
// Result accessors
// ---------------------------------------------------------------------------

SYNTAQLITE_API uint32_t syntaqlite_result_root(SyntaqliteParser* p) {
  if (p->last_status != SYNTAQLITE_PARSE_OK) {
    return SYNTAQLITE_NULL_NODE;
  }
  return p->ctx.root;
}

SYNTAQLITE_API uint32_t syntaqlite_result_recovery_root(SyntaqliteParser* p) {
  if (p->last_status != SYNTAQLITE_PARSE_ERROR) {
    return SYNTAQLITE_NULL_NODE;
  }
  return p->ctx.root;
}

SYNTAQLITE_API const char* syntaqlite_result_error_msg(SyntaqliteParser* p) {
  return p->error_msg[0] ? p->error_msg : NULL;
}

SYNTAQLITE_API uint32_t syntaqlite_result_error_offset(SyntaqliteParser* p) {
  return p->ctx.error_offset;
}

SYNTAQLITE_API uint32_t syntaqlite_result_error_length(SyntaqliteParser* p) {
  return p->ctx.error_length;
}

SYNTAQLITE_API const SyntaqliteComment* syntaqlite_result_comments(
    SyntaqliteParser* p,
    uint32_t* count) {
  *count = syntaqlite_vec_len(&p->comments);
  return p->comments.data;
}

SYNTAQLITE_API const SyntaqliteParserToken* syntaqlite_result_tokens(
    SyntaqliteParser* p,
    uint32_t* count) {
  *count = syntaqlite_vec_len(&p->tokens);
  return p->tokens.data;
}

// O(1) lookup, backed by the per-token comment index that
// `synq_parser_record_comment` maintains incrementally.  `token_idx ==
// ntokens` targets the orphan bucket in `pending_orphan_leading` —
// the "statement-trailing comment with no owner" case.
static const SyntaqliteComment* token_side_comments(SyntaqliteParser* p,
                                                    uint32_t token_idx,
                                                    uint8_t side,
                                                    uint32_t* count) {
  uint32_t ntokens = syntaqlite_vec_len(&p->tokens);
  SynqTokenComments tc;
  if (token_idx < ntokens) {
    tc = syntaqlite_vec_at(&p->token_comments, token_idx);
  } else if (token_idx == ntokens) {
    tc = p->pending_orphan_leading;
  } else {
    *count = 0;
    return NULL;
  }
  uint32_t start =
      side == SYNQ_COMMENT_LEADING ? tc.leading_first : tc.trailing_first;
  uint32_t cnt =
      side == SYNQ_COMMENT_LEADING ? tc.leading_count : tc.trailing_count;
  if (cnt == 0) {
    *count = 0;
    return NULL;
  }
  *count = cnt;
  return &p->comments.data[start];
}

SYNTAQLITE_API const SyntaqliteComment* syntaqlite_token_leading_comments(
    SyntaqliteParser* p,
    uint32_t token_idx,
    uint32_t* count) {
  return token_side_comments(p, token_idx, SYNQ_COMMENT_LEADING, count);
}

SYNTAQLITE_API const SyntaqliteComment* syntaqlite_token_trailing_comments(
    SyntaqliteParser* p,
    uint32_t token_idx,
    uint32_t* count) {
  return token_side_comments(p, token_idx, SYNQ_COMMENT_TRAILING, count);
}

// Resolve `node_id` to the inclusive token range `[*first_tok, *last_tok]`
// of entries in `p->tokens` that the parser fed to Lemon while reducing
// this node.  O(1): the range is carried alongside the byte extent on
// a single shadow stack and committed per node on reduction.
//
// Works across macro boundaries: since the token-stream unification,
// every shifted terminal (including macro-expansion tokens) has a real
// `token_idx` into `p->tokens` regardless of layer.
SYNTAQLITE_API int32_t
syntaqlite_node_token_range(SyntaqliteParser* p,
                            uint32_t node_id,
                            SyntaqliteTokenIdx* first_tok,
                            SyntaqliteTokenIdx* last_tok) {
  if (!p->ctx.collect_node_extents) {
    return 0;
  }
  if (node_id == SYNTAQLITE_NULL_NODE) {
    return 0;
  }
  if (node_id >= syntaqlite_vec_len(&p->ctx.node_extents)) {
    return 0;
  }
  SynqExtentRange r = syntaqlite_vec_at(&p->ctx.node_extents, node_id);
  if (r.first_tok == UINT32_MAX) {
    return 0;
  }
  *first_tok = r.first_tok;
  *last_tok = r.last_tok;
  return 1;
}

SYNTAQLITE_API const SyntaqliteComment* syntaqlite_node_leading_comments(
    SyntaqliteParser* p,
    uint32_t node_id,
    uint32_t* count) {
  SyntaqliteTokenIdx first = 0;
  SyntaqliteTokenIdx last = 0;
  if (!syntaqlite_node_token_range(p, node_id, &first, &last)) {
    *count = 0;
    return NULL;
  }
  return token_side_comments(p, first, SYNQ_COMMENT_LEADING, count);
}

SYNTAQLITE_API const SyntaqliteComment* syntaqlite_node_trailing_comments(
    SyntaqliteParser* p,
    uint32_t node_id,
    uint32_t* count) {
  SyntaqliteTokenIdx first = 0;
  SyntaqliteTokenIdx last = 0;
  if (!syntaqlite_node_token_range(p, node_id, &first, &last)) {
    *count = 0;
    return NULL;
  }
  return token_side_comments(p, last, SYNQ_COMMENT_TRAILING, count);
}

#ifdef SYNTAQLITE_OMIT_MACROS
SYNTAQLITE_API uint32_t syntaqlite_result_macro_count(SyntaqliteParser* p) {
  (void)p;
  return 0;
}
SYNTAQLITE_API SyntaqliteMacroRewrite
syntaqlite_result_macro_rewrite_at(SyntaqliteParser* p, uint32_t idx) {
  (void)p;
  (void)idx;
  return (SyntaqliteMacroRewrite){
      .parent_idx = SYNTAQLITE_MACRO_PARENT_SOURCE,
  };
}
SYNTAQLITE_API uint32_t
syntaqlite_macro_rewrite_arg_segment_count(SyntaqliteParser* p,
                                           uint32_t rewrite_idx) {
  (void)p;
  (void)rewrite_idx;
  return 0;
}
SYNTAQLITE_API SyntaqliteMacroArgSegment
syntaqlite_macro_rewrite_arg_segment_at(SyntaqliteParser* p,
                                        uint32_t rewrite_idx,
                                        uint32_t segment_idx) {
  (void)p;
  (void)rewrite_idx;
  (void)segment_idx;
  return (SyntaqliteMacroArgSegment){0};
}
SYNTAQLITE_API uint32_t
syntaqlite_macro_rewrite_arg_count(SyntaqliteParser* p, uint32_t rewrite_idx) {
  (void)p;
  (void)rewrite_idx;
  return 0;
}
SYNTAQLITE_API SyntaqliteMacroCallArg
syntaqlite_macro_rewrite_arg_at(SyntaqliteParser* p,
                                uint32_t rewrite_idx,
                                uint32_t arg_idx) {
  (void)p;
  (void)rewrite_idx;
  (void)arg_idx;
  return (SyntaqliteMacroCallArg){0};
}
#else
SYNTAQLITE_API uint32_t syntaqlite_result_macro_count(SyntaqliteParser* p) {
  uint32_t total = syntaqlite_vec_len(&p->macro.layers);
  // Entry 0 is the source sentinel; real expansion layers start at 1.
  return total <= 1 ? 0 : total - 1;
}
SYNTAQLITE_API SyntaqliteMacroRewrite
syntaqlite_result_macro_rewrite_at(SyntaqliteParser* p, uint32_t idx) {
  // +1 to skip the source sentinel at index 0.
  uint32_t layer_idx = idx + 1;
  if (layer_idx >= syntaqlite_vec_len(&p->macro.layers)) {
    return (SyntaqliteMacroRewrite){
        .parent_idx = SYNTAQLITE_MACRO_PARENT_SOURCE,
    };
  }
  const SynqExpansionLayer* lyr = &p->macro.layers.data[layer_idx];
  // Internal parent_layer_id 0 = authored source sentinel.  Map it to the
  // public sentinel value; otherwise subtract 1 to account for the skipped
  // source entry.
  uint32_t parent_idx = lyr->parent_layer_id == 0
                            ? SYNTAQLITE_MACRO_PARENT_SOURCE
                            : lyr->parent_layer_id - 1;
  // Resolve the buffer that `call_offset` and every arg offset
  // measure into, so consumers can slice directly without walking
  // the parent chain.  Top-level layers measure into the current
  // statement slice; nested layers measure into the parent layer's
  // expansion buffer.
  const char* parent_buffer;
  uint32_t parent_buffer_len;
  if (lyr->parent_layer_id == 0) {
    parent_buffer = p->stmt_source;
    parent_buffer_len = p->stmt_end_offset - p->stmt_start_offset;
  } else {
    const SynqExpansionLayer* parent =
        &p->macro.layers.data[lyr->parent_layer_id];
    parent_buffer = parent->expansion_data;
    parent_buffer_len = parent->expansion_len;
  }
  return (SyntaqliteMacroRewrite){
      .parent_idx = parent_idx,
      .call_offset = lyr->call_offset,
      .call_length = lyr->call_length,
      .expansion = lyr->expansion_data,
      .expansion_len = lyr->expansion_len,
      .name = lyr->name,
      .name_len = lyr->name_len,
      .def_line = lyr->def_line,
      .def_col = lyr->def_col,
      .body_call_offset = lyr->body_call_offset,
      .body_call_length = lyr->body_call_length,
      .parent_buffer = parent_buffer,
      .parent_buffer_len = parent_buffer_len,
      .is_fallback = lyr->is_fallback,
  };
}

SYNTAQLITE_API uint32_t
syntaqlite_macro_rewrite_arg_segment_count(SyntaqliteParser* p,
                                           uint32_t rewrite_idx) {
  uint32_t layer_idx = rewrite_idx + 1;
  if (layer_idx >= syntaqlite_vec_len(&p->macro.layers))
    return 0;
  return p->macro.layers.data[layer_idx].arg_segment_count;
}

SYNTAQLITE_API SyntaqliteMacroArgSegment
syntaqlite_macro_rewrite_arg_segment_at(SyntaqliteParser* p,
                                        uint32_t rewrite_idx,
                                        uint32_t segment_idx) {
  uint32_t layer_idx = rewrite_idx + 1;
  if (layer_idx >= syntaqlite_vec_len(&p->macro.layers))
    return (SyntaqliteMacroArgSegment){0};
  const SynqExpansionLayer* lyr = &p->macro.layers.data[layer_idx];
  if (segment_idx >= lyr->arg_segment_count)
    return (SyntaqliteMacroArgSegment){0};
  const SynqArgSegment* seg = &lyr->arg_segments[segment_idx];
  // Map internal origin_layer_id (0 = source sentinel) to the public
  // sentinel / rewrite-index scheme used by parent_idx.
  uint32_t origin_parent_idx = seg->origin_layer_id == 0
                                   ? SYNTAQLITE_MACRO_PARENT_SOURCE
                                   : seg->origin_layer_id - 1;
  return (SyntaqliteMacroArgSegment){
      .body_offset = seg->body_offset,
      .body_length = seg->body_length,
      .expansion_offset = seg->sub_offset,
      .expansion_length = seg->sub_length,
      .origin_parent_idx = origin_parent_idx,
      .origin_offset = seg->origin_offset,
      .origin_length = seg->origin_length,
  };
}

SYNTAQLITE_API uint32_t
syntaqlite_macro_rewrite_arg_count(SyntaqliteParser* p, uint32_t rewrite_idx) {
  uint32_t layer_idx = rewrite_idx + 1;
  if (layer_idx >= syntaqlite_vec_len(&p->macro.layers))
    return 0;
  return p->macro.layers.data[layer_idx].arg_count;
}

SYNTAQLITE_API SyntaqliteMacroCallArg
syntaqlite_macro_rewrite_arg_at(SyntaqliteParser* p,
                                uint32_t rewrite_idx,
                                uint32_t arg_idx) {
  uint32_t layer_idx = rewrite_idx + 1;
  if (layer_idx >= syntaqlite_vec_len(&p->macro.layers))
    return (SyntaqliteMacroCallArg){0};
  const SynqExpansionLayer* lyr = &p->macro.layers.data[layer_idx];
  if (arg_idx >= lyr->arg_count)
    return (SyntaqliteMacroCallArg){0};
  const SynqMacroArg* arg = &lyr->args[arg_idx];
  return (SyntaqliteMacroCallArg){
      .offset = arg->offset,
      .length = arg->length,
  };
}
#endif

// ---------------------------------------------------------------------------
// Arena accessors
// ---------------------------------------------------------------------------

SYNTAQLITE_API const void* syntaqlite_parser_node(SyntaqliteParser* p,
                                                  uint32_t node_id) {
  return synq_arena_cptr(&p->ctx.ast, node_id);
}

SYNTAQLITE_API uint32_t syntaqlite_parser_node_count(SyntaqliteParser* p) {
  return syntaqlite_vec_len(&p->ctx.ast.offsets);
}

#ifndef SYNTAQLITE_OMIT_MACROS
static void append_expanded_range(SyntaqliteParser* p,
                                  uint32_t layer_id,
                                  const char* buf,
                                  uint32_t buf_len,
                                  uint32_t start,
                                  uint32_t end);
#endif

SYNTAQLITE_API const char* syntaqlite_parser_full_text(SyntaqliteParser* p,
                                                       uint32_t* out_len) {
  if (out_len) {
    *out_len = p->source_len;
  }
  return p->source;
}

SYNTAQLITE_API const char* syntaqlite_parser_text(SyntaqliteParser* p,
                                                  uint32_t* out_offset,
                                                  uint32_t* out_len) {
  if (p->stmt_start_offset == UINT32_MAX ||
      p->stmt_end_offset <= p->stmt_start_offset ||
      p->stmt_end_offset > p->source_len) {
    if (out_offset)
      *out_offset = 0;
    if (out_len)
      *out_len = 0;
    return NULL;
  }
  if (out_offset)
    *out_offset = p->stmt_start_offset;
  if (out_len)
    *out_len = p->stmt_end_offset - p->stmt_start_offset;
  return p->stmt_source;
}

SYNTAQLITE_API const char* syntaqlite_parser_layer_text(
    SyntaqliteParser* p,
    uint32_t layer_id,
    SyntaqliteLength* out_len) {
  if (out_len) {
    *out_len = 0;
  }
  if (layer_id == 0) {
    // Authored source for the current statement.  Identical to
    // `syntaqlite_parser_text` but typed for the layer-indexed API.
    if (p->stmt_start_offset == UINT32_MAX ||
        p->stmt_end_offset <= p->stmt_start_offset ||
        p->stmt_end_offset > p->source_len) {
      return NULL;
    }
    if (out_len) {
      *out_len = p->stmt_end_offset - p->stmt_start_offset;
    }
    return p->stmt_source;
  }
#ifdef SYNTAQLITE_OMIT_MACROS
  (void)layer_id;
  return NULL;
#else
  if (layer_id >= syntaqlite_vec_len(&p->macro.layers)) {
    return NULL;
  }
  const SynqExpansionLayer* lyr = &p->macro.layers.data[layer_id];
  if (out_len) {
    *out_len = lyr->expansion_len;
  }
  return lyr->expansion_data;
#endif
}

SYNTAQLITE_API const char* syntaqlite_parser_expanded_text(SyntaqliteParser* p,
                                                           uint32_t* out_len) {
#ifdef SYNTAQLITE_OMIT_MACROS
  uint32_t ignored_offset = 0;
  const char* s = syntaqlite_parser_text(p, &ignored_offset, out_len);
  return s ? s : "";
#else
  if (out_len) {
    *out_len = 0;
  }
  if (p->stmt_start_offset == UINT32_MAX ||
      p->stmt_end_offset <= p->stmt_start_offset ||
      p->stmt_end_offset > p->source_len) {
    return "";
  }
  uint32_t stmt_len = p->stmt_end_offset - p->stmt_start_offset;
  syntaqlite_vec_clear(&p->macro.node_expanded_buf);
  append_expanded_range(p, 0, p->stmt_source, stmt_len, 0, stmt_len);
  if (out_len) {
    *out_len = syntaqlite_vec_len(&p->macro.node_expanded_buf);
  }
  return (const char*)p->macro.node_expanded_buf.data;
#endif
}

// ---------------------------------------------------------------------------
// Low-level token-feeding API
// ---------------------------------------------------------------------------

SYNTAQLITE_API int32_t syntaqlite_parser_feed_token(SyntaqliteParser* p,
                                                    uint32_t token_type,
                                                    const char* text,
                                                    uint32_t len) {
  // Deferred reset: clear previous statement's data before processing the
  // first token of the next one.  Lemon was already reinitialized eagerly
  // by stmt_boundary() when the previous statement completed.
  if (p->pending_reset) {
    reset_stmt(p);
    p->pending_reset = 0;
  }

  if (text) {
    // Open the statement at the first non-whitespace byte, matching
    // parser_next (which never sees TK_SPACE because the tokenizer
    // skips it).  Leading TK_COMMENT still opens a statement.
    if (p->stmt_start_offset == UINT32_MAX &&
        token_type != SYNTAQLITE_TK_SPACE) {
      synq_open_statement(p, (uint32_t)(text - p->source));
    }
    // Advance p->offset so set_result_status can finalize stmt_end_offset;
    // parser_next updates it during tokenization but feed_token never
    // touches it otherwise.
    uint32_t tok_end = (uint32_t)(text - p->source) + len;
    if (tok_end > p->offset)
      p->offset = tok_end;
  }

  // Skip whitespace and comments without feeding to Lemon.
  if (synq_token_is_skip(token_type)) {
    if (token_type == SYNTAQLITE_TK_COMMENT && p->collect_tokens && text) {
      synq_parser_record_comment(p, (uint32_t)(text - p->source), len);
    }
    return set_result_status(p, SYNTAQLITE_PARSE_DONE);
  }

  uint32_t layer_offset = text ? (uint32_t)(text - p->stmt_source) : 0;
  int rc = shift_token(p, token_type, text, len, layer_offset);
  if (rc < 0)
    return set_result_status(p, SYNTAQLITE_PARSE_ERROR);

  if (rc == 1) {
    // Bare semicolons (ecmd ::= SEMI.) and error-recovery completions
    // (ecmd ::= error SEMI.) have root == NULL_NODE and may have consumed
    // the next token as an LALR(1) lookahead.  Do NOT reinitialize Lemon —
    // the consumed token is already in Lemon's state and will be processed
    // normally on the next feed_token call.
    if (p->ctx.root == SYNTAQLITE_NULL_NODE) {
      if (p->had_error) {
        p->had_error = 0;
        p->pending_reset = 1;
        return set_result_status(p, SYNTAQLITE_PARSE_ERROR);
      }
      return set_result_status(p, SYNTAQLITE_PARSE_DONE);
    }

    // Real statement — cmdx ::= cmd. fired with SEMI as the lookahead,
    // leaving ecmd ::= cmdx SEMI. pending.  Reinitialize Lemon.
    int32_t status = stmt_boundary(p);
    p->pending_reset = 1;
    return set_result_status(p, status);
  }

  return set_result_status(p, SYNTAQLITE_PARSE_DONE);
}

SYNTAQLITE_API uint32_t syntaqlite_parser_expected_tokens(SyntaqliteParser* p,
                                                          uint32_t* out_tokens,
                                                          uint32_t out_cap) {
  if (p == NULL || p->dialect.tmpl == NULL ||
      p->dialect.tmpl->parser_expected_tokens == NULL) {
    return 0;
  }
  return p->dialect.tmpl->parser_expected_tokens(p->lemon, out_tokens, out_cap);
}

SYNTAQLITE_API SyntaqliteCompletionContext
syntaqlite_parser_completion_context(SyntaqliteParser* p) {
  if (p == NULL || p->dialect.tmpl == NULL ||
      p->dialect.tmpl->parser_completion_context == NULL) {
    return SYNTAQLITE_COMPLETION_CONTEXT_UNKNOWN;
  }
  return (SyntaqliteCompletionContext)
      p->dialect.tmpl->parser_completion_context(p->lemon);
}

SYNTAQLITE_API int32_t syntaqlite_parser_finish(SyntaqliteParser* p) {
  if (p->pending_reset) {
    // Nothing pending after a completed statement — done.
    p->pending_reset = 0;
    return set_result_status(p, SYNTAQLITE_PARSE_DONE);
  }
  return finish_input(p);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

SYNTAQLITE_API int32_t syntaqlite_parser_set_trace(SyntaqliteParser* p,
                                                   uint32_t enable) {
  if (p->sealed)
    return SYNTAQLITE_ERR_ALREADY_USED;
  p->trace = enable;
  if (enable) {
    SYNQ_PARSER_TRACE(p->dialect.tmpl, stderr, "parser> ");
  } else {
    SYNQ_PARSER_TRACE(p->dialect.tmpl, NULL, NULL);
  }
  return SYNTAQLITE_OK;
}

SYNTAQLITE_API int32_t syntaqlite_parser_set_collect_tokens(SyntaqliteParser* p,
                                                            uint32_t enable) {
  if (p->sealed)
    return SYNTAQLITE_ERR_ALREADY_USED;
  p->collect_tokens = enable;
  return SYNTAQLITE_OK;
}

SYNTAQLITE_API int32_t syntaqlite_parser_set_macro_fallback(SyntaqliteParser* p,
                                                            uint32_t enable) {
  if (p->sealed)
    return SYNTAQLITE_ERR_ALREADY_USED;
#ifdef SYNTAQLITE_OMIT_MACROS
  (void)enable;
  return SYNTAQLITE_ERR_OMITTED;
#else
  p->macro.macro_fallback = enable;
  return SYNTAQLITE_OK;
#endif
}

SYNTAQLITE_API int32_t
syntaqlite_parser_set_collect_node_extents(SyntaqliteParser* p,
                                           uint32_t enable) {
  if (p->sealed)
    return SYNTAQLITE_ERR_ALREADY_USED;
  p->ctx.collect_node_extents = enable;
  return SYNTAQLITE_OK;
}

SYNTAQLITE_API const char* syntaqlite_parser_node_text(SyntaqliteParser* p,
                                                       uint32_t node_id,
                                                       uint32_t* out_len,
                                                       uint32_t* out_offset) {
  if (out_len) {
    *out_len = 0;
  }
  if (out_offset) {
    *out_offset = 0;
  }
  if (!p->ctx.collect_node_extents) {
    return NULL;
  }
  if (node_id >= syntaqlite_vec_len(&p->ctx.node_extents)) {
    return NULL;
  }
  SynqExtentRange r = syntaqlite_vec_at(&p->ctx.node_extents, node_id);
  // Sentinel `(UINT32_MAX, 0)` → not recorded.
  uint32_t stmt_len = p->stmt_end_offset > p->stmt_start_offset
                          ? p->stmt_end_offset - p->stmt_start_offset
                          : 0;
  if (r.root_start > r.root_end || r.root_end > stmt_len) {
    return NULL;
  }
  if (out_len) {
    *out_len = r.root_end - r.root_start;
  }
  if (out_offset) {
    *out_offset = r.root_start;
  }
  return p->stmt_source + r.root_start;
}

#ifndef SYNTAQLITE_OMIT_MACROS
static void append_expanded_range(SyntaqliteParser* p,
                                  uint32_t layer_id,
                                  const char* buf,
                                  uint32_t buf_len,
                                  uint32_t start,
                                  uint32_t end) {
  if (start > buf_len)
    start = buf_len;
  if (end > buf_len)
    end = buf_len;
  uint32_t cursor = start;
  uint32_t nlayers = syntaqlite_vec_len(&p->macro.layers);
  for (;;) {
    // Find the next child layer (parent == layer_id) whose call site
    // begins at or after `cursor` and lies fully within `[start, end)`.
    uint32_t best_child = 0;
    uint32_t best_offset = UINT32_MAX;
    for (uint32_t i = 1; i < nlayers; i++) {
      const SynqExpansionLayer* lyr = &p->macro.layers.data[i];
      if (lyr->parent_layer_id != layer_id)
        continue;
      if (lyr->call_offset < cursor)
        continue;
      if (lyr->call_offset + lyr->call_length > end)
        continue;
      if (lyr->call_offset < best_offset) {
        best_offset = lyr->call_offset;
        best_child = i;
      }
    }
    if (best_child == 0)
      break;
    const SynqExpansionLayer* child = &p->macro.layers.data[best_child];
    if (best_offset > cursor) {
      uint32_t n = best_offset - cursor;
      syntaqlite_vec_push_n(&p->macro.node_expanded_buf, buf + cursor, n,
                            p->mem);
    }
    append_expanded_range(p, best_child, child->expansion_data,
                          child->expansion_len, 0, child->expansion_len);
    cursor = best_offset + child->call_length;
  }
  if (end > cursor) {
    uint32_t n = end - cursor;
    syntaqlite_vec_push_n(&p->macro.node_expanded_buf, buf + cursor, n, p->mem);
  }
}
#endif  // !SYNTAQLITE_OMIT_MACROS

SYNTAQLITE_API const char* syntaqlite_parser_node_expanded_text(
    SyntaqliteParser* p,
    uint32_t node_id,
    uint32_t* out_len) {
#ifdef SYNTAQLITE_OMIT_MACROS
  // Without macros, expanded text == authored text.
  return syntaqlite_parser_node_text(p, node_id, out_len, NULL);
#else
  if (out_len) {
    *out_len = 0;
  }
  if (!p->ctx.collect_node_extents) {
    return NULL;
  }
  if (node_id >= syntaqlite_vec_len(&p->ctx.node_expanded_extents)) {
    return NULL;
  }

  SynqNodeExpandedExtent e =
      syntaqlite_vec_at(&p->ctx.node_expanded_extents, node_id);
  if (e.length > 0) {
    const char* buf = e.layer_id == 0
                          ? p->stmt_source
                          : p->macro.layers.data[e.layer_id].expansion_data;
    if (out_len) {
      *out_len = e.length;
    }
    return buf + e.offset;
  }

  if (node_id >= syntaqlite_vec_len(&p->ctx.node_extents)) {
    return NULL;
  }
  SynqExtentRange r = syntaqlite_vec_at(&p->ctx.node_extents, node_id);
  uint32_t stmt_len = p->stmt_end_offset > p->stmt_start_offset
                          ? p->stmt_end_offset - p->stmt_start_offset
                          : 0;
  if (r.root_start > r.root_end || r.root_end > stmt_len) {
    return NULL;
  }
  syntaqlite_vec_clear(&p->macro.node_expanded_buf);
  append_expanded_range(p, 0, p->stmt_source, stmt_len, r.root_start,
                        r.root_end);
  if (out_len) {
    *out_len = syntaqlite_vec_len(&p->macro.node_expanded_buf);
  }
  return (const char*)p->macro.node_expanded_buf.data;
#endif
}

SYNTAQLITE_API int syntaqlite_node_is_macro_free(SyntaqliteParser* p,
                                                 uint32_t node_id) {
#ifdef SYNTAQLITE_OMIT_MACROS
  (void)p;
  (void)node_id;
  return 1;  // No macros → all nodes are macro-free.
#else
  if (!p->ctx.collect_node_extents) {
    return 0;
  }
  if (node_id >= syntaqlite_vec_len(&p->ctx.node_expanded_extents)) {
    return 0;
  }
  SynqNodeExpandedExtent e =
      syntaqlite_vec_at(&p->ctx.node_expanded_extents, node_id);
  // length == 0 is the sentinel for epsilon or multi-layer nodes.
  return e.length > 0 && e.layer_id == 0;
#endif
}

// ---------------------------------------------------------------------------
// SYNTAQLITE_OMIT_MACROS stubs for span/traceback APIs
// ---------------------------------------------------------------------------
//
// When macros are compiled out, parser_spans.c is empty.  These stubs
// provide the same public API with trivial implementations: all spans
// are in layer 0 (source), so span_text is a direct source slice and
// traceback returns NULL (no expansion frames to report).

#ifdef SYNTAQLITE_OMIT_MACROS

SYNTAQLITE_API const char* syntaqlite_parser_span_text(
    SyntaqliteParser* p,
    const SyntaqliteTextSpan* span,
    uint32_t* out_len,
    uint32_t* out_offset) {
  if (out_offset)
    *out_offset = 0;
  if (!span || span->length == 0) {
    *out_len = 0;
    return NULL;
  }
  uint32_t stmt_len = p->stmt_end_offset > p->stmt_start_offset
                          ? p->stmt_end_offset - p->stmt_start_offset
                          : 0;
  if (span->offset + span->length > stmt_len) {
    *out_len = 0;
    return NULL;
  }
  *out_len = span->length;
  if (out_offset)
    *out_offset = span->offset;
  return p->stmt_source + span->offset;
}

SYNTAQLITE_API const char* syntaqlite_parser_span_expanded_text(
    SyntaqliteParser* p,
    const SyntaqliteTextSpan* span,
    uint32_t* out_len) {
  return syntaqlite_parser_span_text(p, span, out_len, NULL);
}

SYNTAQLITE_API const SyntaqliteTracebackFrame* syntaqlite_parser_traceback(
    SyntaqliteParser* p,
    const SyntaqliteTextSpan* sp,
    uint32_t* out_count) {
  (void)p;
  (void)sp;
  if (out_count)
    *out_count = 0;
  return NULL;
}

SYNTAQLITE_API int32_t
syntaqlite_parser_set_macro_lookup(SyntaqliteParser* p,
                                   SyntaqliteMacroLookupFn fn,
                                   void* user_data) {
  (void)p;
  (void)fn;
  (void)user_data;
  return SYNTAQLITE_ERR_OMITTED;
}

#endif  // SYNTAQLITE_OMIT_MACROS
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/parser.c ======== */

/* ======== begin: csrc/parser_dump.c ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// AST dump — indented text representation of a parsed tree.
//
// Split out of parser.c for size/isolation: this code is self-contained and
// only reaches into the parser's arena + dialect field metadata.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>


typedef SYNQ_VEC(char) DumpBuf;

static void dump_append(DumpBuf* b,
                        SyntaqliteMemMethods mem,
                        const char* s,
                        uint32_t n) {
  syntaqlite_vec_push_n(b, s, n, mem);
}

SYNQ_PRINTF(3, 4)
static void dump_printf(DumpBuf* b,
                        SyntaqliteMemMethods mem,
                        const char* fmt,
                        ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  syntaqlite_vec_ensure(b, b->count + (uint32_t)n + 1, mem);
  va_start(ap, fmt);
  vsnprintf(b->data + b->count, (uint32_t)n + 1, fmt, ap);
  va_end(ap);
  b->count += (uint32_t)n;
}

static void dump_indent(DumpBuf* b, SyntaqliteMemMethods mem, uint32_t indent) {
  for (uint32_t i = 0; i < indent; i++)
    dump_append(b, mem, "  ", 2);
}

static void dump_node_recursive(DumpBuf* b,
                                SyntaqliteParser* p,
                                uint32_t node_id,
                                uint32_t indent) {
  if (node_id == SYNTAQLITE_NULL_NODE)
    return;
  uint32_t count = syntaqlite_vec_len(&p->ctx.ast.offsets);
  if (node_id >= count)
    return;

  const uint8_t* raw = synq_arena_cptr(&p->ctx.ast, node_id);
  uint32_t tag;
  memcpy(&tag, raw, sizeof(tag));

  const SyntaqliteDialectTemplate* g = p->dialect.tmpl;
  if (tag >= g->node_count)
    return;

  const char* name = g->node_names[tag];
  uint8_t field_count = g->field_meta_counts[tag];
  SyntaqliteMemMethods mem = p->mem;

  // List node: no field descriptors, has tag + count header.
  if (field_count == 0 && tag != 0) {
    SynqListHeader hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    dump_indent(b, mem, indent);
    dump_printf(b, mem, "%s [%u items]\n", name, hdr.count);
    const uint32_t* children = (const uint32_t*)(raw + sizeof(SynqListHeader));
    for (uint32_t i = 0; i < hdr.count; i++) {
      dump_node_recursive(b, p, children[i], indent + 1);
    }
    return;
  }

  dump_indent(b, mem, indent);
  dump_printf(b, mem, "%s\n", name);

  if (field_count == 0)
    return;
  const SyntaqliteFieldMeta* fields = g->field_meta[tag];

  for (uint8_t fi = 0; fi < field_count; fi++) {
    const SyntaqliteFieldMeta* fm = &fields[fi];
    const uint8_t* field_ptr = raw + fm->offset;

    switch (fm->kind) {
      case SYNTAQLITE_FIELD_NODE_ID: {
        uint32_t child_id;
        memcpy(&child_id, field_ptr, sizeof(child_id));
        dump_indent(b, mem, indent + 1);
        if (child_id == SYNTAQLITE_NULL_NODE) {
          dump_printf(b, mem, "%s: (none)\n", fm->name);
        } else {
          dump_printf(b, mem, "%s:\n", fm->name);
          dump_node_recursive(b, p, child_id, indent + 2);
        }
        break;
      }
      case SYNTAQLITE_FIELD_SPAN: {
        SyntaqliteTextSpan sp;
        memcpy(&sp, field_ptr, sizeof(sp));
        dump_indent(b, mem, indent + 1);
        if (sp.length == 0) {
          dump_printf(b, mem, "%s: (none)\n", fm->name);
        } else {
          const char* base =
#ifdef SYNTAQLITE_OMIT_MACROS
              p->stmt_source;
#else
              sp._layer_id == 0
                  ? p->stmt_source
                  : p->macro.layers.data[sp._layer_id].expansion_data;
#endif
          const char* text = base + sp.offset;
          char q = syntaqlite_span_quote_char(sp);
          dump_printf(b, mem, "%s: \"", fm->name);
          if (q != 0 && q != '[' && memchr(text, q, sp.length)) {
            // Quoted identifier containing its escape (doubled quote
            // char): dump the identifier *value* so equal names dump
            // equally regardless of the quote style that encoded them.
            for (uint32_t i = 0; i < sp.length; i++) {
              dump_append(b, mem, &text[i], 1);
              if (text[i] == q && i + 1 < sp.length && text[i + 1] == q)
                i++;
            }
          } else {
            dump_append(b, mem, text, sp.length);
          }
          dump_append(b, mem, "\"\n", 2);
        }
        break;
      }
      case SYNTAQLITE_FIELD_BOOL: {
        uint32_t val;
        memcpy(&val, field_ptr, sizeof(val));
        dump_indent(b, mem, indent + 1);
        dump_printf(b, mem, "%s: %s\n", fm->name, val ? "TRUE" : "FALSE");
        break;
      }
      case SYNTAQLITE_FIELD_FLAGS: {
        uint8_t val = *field_ptr;
        // Mask to defined bits only — upper bits may contain struct padding.
        if (fm->display_count < 8)
          val &= (uint8_t)((1u << fm->display_count) - 1);
        dump_indent(b, mem, indent + 1);
        dump_printf(b, mem, "%s: ", fm->name);
        if (val == 0) {
          dump_append(b, mem, "(none)", 6);
        } else {
          int first = 1;
          for (int bit = 0; bit < fm->display_count; bit++) {
            if (val & (1 << bit)) {
              const char* flag_name = fm->display ? fm->display[bit] : "?";
              if (flag_name[0] == '\0')
                continue;
              if (!first)
                dump_append(b, mem, " ", 1);
              dump_append(b, mem, flag_name, (uint32_t)strlen(flag_name));
              first = 0;
            }
          }
          if (first)
            dump_append(b, mem, "(none)", 6);
        }
        dump_append(b, mem, "\n", 1);
        break;
      }
      case SYNTAQLITE_FIELD_ENUM: {
        uint32_t val;
        memcpy(&val, field_ptr, sizeof(val));
        dump_indent(b, mem, indent + 1);
        const char* label =
            (val < fm->display_count && fm->display) ? fm->display[val] : "?";
        dump_printf(b, mem, "%s: %s\n", fm->name, label);
        break;
      }
    }
  }
}

SYNTAQLITE_API char* syntaqlite_dump_node(SyntaqliteParser* p,
                                          uint32_t node_id,
                                          uint32_t indent) {
  DumpBuf buf;
  syntaqlite_vec_init(&buf);
  dump_node_recursive(&buf, p, node_id, indent);
  syntaqlite_vec_push(&buf, '\0', p->mem);
  return buf.data;
}
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/parser_dump.c ======== */

/* ======== begin: syntaqlite_dialect/extent_hooks.h ======== */
#ifndef SYNTAQLITE_INTERNAL_EXTENT_HOOKS_H
#define SYNTAQLITE_INTERNAL_EXTENT_HOOKS_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Per-node extent tracking hooks called from the Lemon-generated
// parser via the post-lemon patch step in
// `parser_pipeline::patch_generated_parser_files`.  The macros bridge
// Lemon's `yyParser` (which carries `%extra_context SynqParseCtx*
// pCtx`) to the underlying `synq_extent_on_*` functions.



#ifdef __cplusplus
extern "C" {
#endif

// Called from the top of Lemon's `yy_shift` for every terminal shift.
// Pushes a `(root_start, root_end)` range onto the shadow stack when
// per-node extent tracking is enabled.
SYNTAQLITE_DIALECT_API void synq_extent_on_shift(SynqParseCtx* pCtx,
                                                 unsigned int major,
                                                 const SynqParseToken* token);

// Called from the top of Lemon's `yy_reduce` for every rule reduction,
// before the user action switch.  Pops `nrhs` entries from the shadow
// stack and pushes their merged range.
SYNTAQLITE_DIALECT_API void synq_extent_on_reduce(SynqParseCtx* pCtx,
                                                  unsigned int nrhs);

// Called from grammar actions of rules whose parent rule is
// {NEVER-REDUCE}: merges the shadow-stack entry directly below the top
// into the top (both authored and expanded ranges), standing in for the
// parent reduction that will never run.  Follow with
// `synq_extent_record` to re-record the widened extent on the node.
SYNTAQLITE_DIALECT_API void synq_extent_fold_below_into_top(SynqParseCtx* pCtx);

// Lemon stores `yyRuleInfoNRhs[r]` as the negative of the rule's RHS
// symbol count, so the reduce macro negates it to recover `nrhs`.
#define synq_on_shift(yypParser, yyMajor, yyMinor_ptr)             \
  synq_extent_on_shift((yypParser)->pCtx, (unsigned int)(yyMajor), \
                       (yyMinor_ptr))

#define synq_on_reduce(yypParser, yyruleno) \
  synq_extent_on_reduce((yypParser)->pCtx,  \
                        (unsigned int)(-yyRuleInfoNRhs[yyruleno]))

#ifdef __cplusplus
}
#endif


#endif  /* SYNTAQLITE_INTERNAL_EXTENT_HOOKS_H */
/* ======== end: syntaqlite_dialect/extent_hooks.h ======== */

/* ======== begin: csrc/parser_extents.c ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Per-node extent tracking hooks, invoked from Lemon's yy_shift / yy_reduce
// via the macros in extent_hooks.h.  Operates on SynqParseCtx* — conceptually
// part of the AST builder, split into its own file to keep ast_builder.h
// declaration-only.


// ---------------------------------------------------------------------------
// Per-node extent tracking hooks
// ---------------------------------------------------------------------------
//
// When enabled, two parallel shadow stacks mirror Lemon's symbol stack:
//
//   * `extent_stack` carries both the merged *authored* byte range in
//     root-source coordinates (used by `syntaqlite_parser_node_text`)
//     and the inclusive token-index range into `p->tokens` (used by
//     `syntaqlite_node_token_range`).  Macro tokens push the outermost
//     call-site byte range stashed in `begin_macro_expansion` with
//     their real `token_idx` (since the token-stream unification,
//     every shifted terminal has an index regardless of layer).
//     Epsilon pushes a sentinel that is neutral under min/max merging
//     for both ranges.
//
//   * `expanded_stack` tracks the merged *expanded* range in the
//     tokens' own layer — used by
//     `syntaqlite_parser_node_expanded_text`.  Same-layer merges keep
//     the layer; mixed-layer merges collapse to the sentinel
//     `(length=0)`, since no contiguous expansion slice can represent
//     a node whose tokens cross layers.
//
// Independently, `straddle_stack` (a lightweight uint32_t vec) tracks
// macro_root per Lemon stack symbol for O(1) straddle detection.  It
// is lazily initialized on first macro use via `lemon_depth`; without
// macros the only cost is one integer increment/decrement per
// shift/reduce.

// Merge `e` into `acc`: min/max over the authored byte range, min/max
// over the token-index range with UINT32_MAX as the "no tokens"
// sentinel on either side.
static void synq_extent_merge(SynqExtentRange* acc, SynqExtentRange e) {
  if (e.root_start < acc->root_start) {
    acc->root_start = e.root_start;
  }
  if (e.root_end > acc->root_end) {
    acc->root_end = e.root_end;
  }
  if (e.first_tok != UINT32_MAX) {
    if (acc->first_tok == UINT32_MAX || e.first_tok < acc->first_tok) {
      acc->first_tok = e.first_tok;
    }
    if (acc->last_tok == UINT32_MAX || e.last_tok > acc->last_tok) {
      acc->last_tok = e.last_tok;
    }
  }
}

// Merge `e` into `acc` in expanded-layer coordinates: same-layer merges
// union the ranges, epsilon ({0,0,0}) is neutral, and cross-layer
// combinations poison `acc` (SYNQ_CROSS_LAYER), which then absorbs all
// further merges.
static void synq_expanded_merge(SynqNodeExpandedExtent* acc,
                                SynqNodeExpandedExtent e) {
  if (acc->layer_id == SYNQ_CROSS_LAYER) {
    return;  // already poisoned
  }
  if (e.layer_id == SYNQ_CROSS_LAYER) {
    *acc = e;  // propagate poison
    return;
  }
  if (e.length == 0) {
    return;  // epsilon
  }
  if (acc->length == 0) {
    *acc = e;
    return;
  }
  if (acc->layer_id != e.layer_id) {
    *acc = (SynqNodeExpandedExtent){0, 0, SYNQ_CROSS_LAYER};
    return;
  }
  uint32_t start = acc->offset < e.offset ? acc->offset : e.offset;
  uint32_t end_a = acc->offset + acc->length;
  uint32_t end_b = e.offset + e.length;
  uint32_t end = end_a > end_b ? end_a : end_b;
  acc->offset = start;
  acc->length = end - start;
}

void synq_extent_record_list_append(SynqParseCtx* ctx,
                                    uint32_t list_id,
                                    uint32_t child) {
  if (!ctx->collect_node_extents)
    return;
  SynqExtentRange range = syntaqlite_vec_at(&ctx->node_extents, child);
  SynqNodeExpandedExtent expanded =
      syntaqlite_vec_at(&ctx->node_expanded_extents, child);
  if (list_id < syntaqlite_vec_len(&ctx->node_extents)) {
    synq_extent_merge(&syntaqlite_vec_at(&ctx->node_extents, list_id), range);
    synq_expanded_merge(
        &syntaqlite_vec_at(&ctx->node_expanded_extents, list_id), expanded);
  } else {
    syntaqlite_vec_push(&ctx->node_extents, range, ctx->mem);
    syntaqlite_vec_push(&ctx->node_expanded_extents, expanded, ctx->mem);
  }
}

void synq_extent_on_shift(SynqParseCtx* pCtx,
                          unsigned int major,
                          const SynqParseToken* token) {
  (void)major;

  pCtx->lemon_depth++;

  // Straddle stack: only active after first macro (macro_root_layer > 0).
  if (pCtx->macro_root_layer) {
    uint32_t mr = (token->layer_id != 0) ? pCtx->macro_root_layer : 0;
    syntaqlite_vec_push(&pCtx->straddle_stack, mr, pCtx->mem);
  }

  if (!pCtx->collect_node_extents) {
    return;
  }
  SynqExtentRange r;
  if (token->layer_id == 0) {
    r.root_start = token->offset;
    r.root_end = token->offset + token->n;
  } else {
    r.root_start = pCtx->macro_root_start;
    r.root_end = pCtx->macro_root_end;
  }
  // Token-index range: valid when the shifted token has a real index
  // in `p->tokens` (all shifted terminals since the token-stream
  // unification, regardless of layer).  UINT32_MAX means "no token
  // recorded" (collect_tokens off, or layer-N shift with no index).
  if (token->token_idx == 0xFFFFFFFFu) {
    r.first_tok = UINT32_MAX;
    r.last_tok = UINT32_MAX;
  } else {
    r.first_tok = token->token_idx;
    r.last_tok = token->token_idx;
  }
  syntaqlite_vec_push(&pCtx->extent_stack, r, pCtx->mem);

  SynqNodeExpandedExtent e = {
      .offset = token->offset,
      .length = token->n,
      .layer_id = token->layer_id,
  };
  syntaqlite_vec_push(&pCtx->expanded_stack, e, pCtx->mem);
}

void synq_extent_on_reduce(SynqParseCtx* pCtx, unsigned int nrhs) {
  // Reduce pops nrhs symbols and pushes 1: net change = 1 - nrhs.
  pCtx->lemon_depth = pCtx->lemon_depth + 1 - nrhs;

  // Straddle detection on the lightweight stack.
  if (pCtx->macro_root_layer) {
    uint32_t slen = syntaqlite_vec_len(&pCtx->straddle_stack);
    if (!pCtx->has_macro_straddle) {
      uint32_t first = SYNQ_STRADDLE_NEUTRAL;
      for (uint32_t i = slen - nrhs; i < slen; i++) {
        uint32_t v = syntaqlite_vec_at(&pCtx->straddle_stack, i);
        if (v == SYNQ_STRADDLE_NEUTRAL)
          continue;
        if (first == SYNQ_STRADDLE_NEUTRAL) {
          first = v;
        } else if (v != first) {
          pCtx->has_macro_straddle = 1;
          break;
        }
      }
    }
    syntaqlite_vec_truncate(&pCtx->straddle_stack, slen - nrhs);
    syntaqlite_vec_push(&pCtx->straddle_stack, SYNQ_STRADDLE_NEUTRAL,
                        pCtx->mem);
  }

  if (!pCtx->collect_node_extents) {
    return;
  }
  uint32_t len = syntaqlite_vec_len(&pCtx->extent_stack);

  // Merge both the authored byte range and the token-index range in a
  // single pass.  Each is tracked with its own sentinel
  // (byte: root_start==UINT32_MAX && root_end==0; token: first_tok==UINT32_MAX)
  // so a node that reduced over macro-expansion-only tokens keeps its
  // byte range (from the call site) even when token indices are absent,
  // and vice versa.
  SynqExtentRange merged = {UINT32_MAX, 0, UINT32_MAX, UINT32_MAX};
  for (uint32_t i = len - nrhs; i < len; i++) {
    synq_extent_merge(&merged, syntaqlite_vec_at(&pCtx->extent_stack, i));
  }
  syntaqlite_vec_truncate(&pCtx->extent_stack, len - nrhs);
  syntaqlite_vec_push(&pCtx->extent_stack, merged, pCtx->mem);

  SynqNodeExpandedExtent exp_merged = {0, 0, 0};
  for (uint32_t i = len - nrhs; i < len; i++) {
    synq_expanded_merge(&exp_merged,
                        syntaqlite_vec_at(&pCtx->expanded_stack, i));
  }
  syntaqlite_vec_truncate(&pCtx->expanded_stack, len - nrhs);
  syntaqlite_vec_push(&pCtx->expanded_stack, exp_merged, pCtx->mem);
}

void synq_extent_fold_below_into_top(SynqParseCtx* pCtx) {
  if (!pCtx->collect_node_extents) {
    return;
  }
  uint32_t len = syntaqlite_vec_len(&pCtx->extent_stack);
  if (len < 2) {
    return;
  }
  synq_extent_merge(&syntaqlite_vec_at(&pCtx->extent_stack, len - 1),
                    syntaqlite_vec_at(&pCtx->extent_stack, len - 2));
  synq_expanded_merge(&syntaqlite_vec_at(&pCtx->expanded_stack, len - 1),
                      syntaqlite_vec_at(&pCtx->expanded_stack, len - 2));
}
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/parser_extents.c ======== */

/* ======== begin: csrc/util.h ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
#ifndef SYNTAQLITE_INTERNAL_UTIL_H
#define SYNTAQLITE_INTERNAL_UTIL_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Small shared helpers used across the parser C sources.


#include <stdint.h>

// Case-insensitive name comparison.
static inline int synq_name_eq_ci(const char* a,
                                  uint32_t alen,
                                  const char* b,
                                  uint32_t blen) {
  if (alen != blen)
    return 0;
  for (uint32_t i = 0; i < alen; i++) {
    uint8_t ca = (uint8_t)a[i], cb = (uint8_t)b[i];
    if (ca >= 'A' && ca <= 'Z')
      ca += 32;
    if (cb >= 'A' && cb <= 'Z')
      cb += 32;
    if (ca != cb)
      return 0;
  }
  return 1;
}


#endif  /* SYNTAQLITE_INTERNAL_UTIL_H */
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/util.h ======== */

/* ======== begin: csrc/parser_macros.c ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Macro expansion pipeline: arg scanning, lookup callback dispatch,
// template expansion, layer lifecycle, and straddle diagnostics.
//
// Compiled out entirely when SYNTAQLITE_OMIT_MACROS is defined.
//
// Span resolution and traceback live in parser_spans.c.
// Per-node extent tracking hooks live in parser_extents.c.
// Cross-file helpers are declared in csrc/parser_internal.h.

#ifndef SYNTAQLITE_OMIT_MACROS

#include <stdio.h>
#include <string.h>



// Forward declarations — defined later in this file but called from
// expand_and_feed (nested expansion) and synq_parser_expand_and_feed_macro.
static void begin_macro_expansion(SyntaqliteParser* p,
                                  uint32_t call_offset,
                                  uint32_t call_length,
                                  const char* name,
                                  uint32_t name_len);
static void synq_end_macro(SyntaqliteParser* p);

// Forward-scan an expansion buffer past whitespace and comments and return
// the next significant token. `*out_pos` is updated to that token's offset;
// `*out_type` and the return value give its type and length. Returns 0
// (with *out_type == 0) at end-of-buffer or tokenizer failure.
//
// Comments inside expansion buffers are recorded to `p->comments` with
// `layer_id = p->ctx.layer_id` (> 0) so consumers that want every
// comment anywhere in the tree — e.g. authored-source + expansion-body
// — can find them via `syntaqlite_token_leading_comments`.  Callers
// filtering to user-authored comments check `Comment.layer_id == 0`.
static int64_t synq_macro_skip(SyntaqliteParser* p,
                               const unsigned char* z,
                               uint32_t buf_len,
                               uint32_t* out_pos,
                               uint32_t* out_type) {
  uint32_t pos = *out_pos;
  while (pos < buf_len) {
    uint32_t ttype = 0;
    int64_t tlen = SynqSqliteGetTokenVersionWrapped(
        &p->dialect, p->macro.macro_fallback, z + pos, &ttype);
    if (tlen <= 0)
      break;
    if (!synq_token_is_skip(ttype)) {
      *out_pos = pos;
      *out_type = ttype;
      return tlen;
    }
    if (ttype == SYNTAQLITE_TK_COMMENT && p->collect_tokens) {
      synq_parser_record_comment(p, pos, (uint32_t)tlen);
    }
    pos += (uint32_t)tlen;
  }
  *out_pos = pos;
  *out_type = 0;
  return 0;
}

// ---------------------------------------------------------------------------
// Macro argument scanning
// ---------------------------------------------------------------------------

// Scan balanced parens after '!' and split into comma-separated args.
// Returns arg count on success, 0 if not a valid macro call.
// `source`/`source_len` is the buffer being scanned (may be original source
// or an expansion buffer for nested macros).
uint32_t synq_parser_scan_macro_args(SyntaqliteParser* p,
                                     const char* source,
                                     uint32_t source_len,
                                     uint32_t bang_offset,
                                     SynqMacroArg* out_args,
                                     uint32_t max_args,
                                     uint32_t* out_end_offset) {
  const unsigned char* z = (const unsigned char*)source;
  uint32_t pos = bang_offset + 1;  // skip '!'

  // Expect LP.
  uint32_t ttype = 0;
  int64_t tlen = SynqSqliteGetTokenVersionWrapped(
      &p->dialect, p->macro.macro_fallback, z + pos, &ttype);
  if (tlen <= 0 || ttype != SYNTAQLITE_TK_LP)
    return 0;
  pos += (uint32_t)tlen;

  // Check for empty args: macro!()
  ttype = 0;
  tlen = SynqSqliteGetTokenVersionWrapped(&p->dialect, p->macro.macro_fallback,
                                          z + pos, &ttype);
  if (tlen > 0 && ttype == SYNTAQLITE_TK_RP) {
    *out_end_offset = pos + (uint32_t)tlen;
    return 0;
  }

  uint32_t arg_count = 0;
  uint32_t depth = 1;
  uint32_t arg_start = pos;
  uint32_t arg_end = pos;  // end of last significant token in current arg

  while (pos < source_len && depth > 0) {
    ttype = 0;
    tlen = SynqSqliteGetTokenVersionWrapped(
        &p->dialect, p->macro.macro_fallback, z + pos, &ttype);
    if (tlen <= 0)
      return 0;

    int is_skip = synq_token_is_skip(ttype);

    // The fallback-macro path stores the whole call as a single
    // TK_ID, so the main token loop in parser.c never sees tokens
    // inside the call and never calls `synq_parser_record_comment`
    // for them. Record them here instead so consumers that ask
    // "are there any comments in byte range [call_off, call_end)?"
    // (notably the formatter's structured-args bail) see the truth.
    if (ttype == SYNTAQLITE_TK_COMMENT && p->collect_tokens) {
      synq_parser_record_comment(p, pos, (uint32_t)tlen);
    }

    if (ttype == SYNTAQLITE_TK_LP) {
      depth++;
    } else if (ttype == SYNTAQLITE_TK_RP) {
      depth--;
      if (depth == 0) {
        if (arg_count < max_args) {
          out_args[arg_count].offset = arg_start;
          out_args[arg_count].length = arg_end - arg_start;
        }
        arg_count++;
        *out_end_offset = pos + (uint32_t)tlen;
        return arg_count;
      }
    } else if (depth == 1 && ttype == SYNTAQLITE_TK_COMMA) {
      if (arg_count < max_args) {
        out_args[arg_count].offset = arg_start;
        out_args[arg_count].length = arg_end - arg_start;
      }
      arg_count++;
      arg_start = pos + (uint32_t)tlen;
      arg_end = arg_start;
    } else if (ttype == SYNTAQLITE_TK_SEMI) {
      return 0;
    }

    // Trim leading whitespace/comments by advancing arg_start.
    // Trim trailing implicitly: arg_end only advances past significant tokens.
    if (depth >= 1 && is_skip && pos == arg_start) {
      arg_start = pos + (uint32_t)tlen;
      arg_end = arg_start;
    } else if (!is_skip) {
      arg_end = pos + (uint32_t)tlen;
    }

    pos += (uint32_t)tlen;
  }

  return 0;  // Unbalanced parens.
}

// ---------------------------------------------------------------------------
// Macro state + layer lifecycle helpers
// ---------------------------------------------------------------------------

void synq_macro_state_init(SynqMacroState* m) {
  syntaqlite_vec_init(&m->expand_buf);
  syntaqlite_vec_init(&m->body_buf);
  syntaqlite_vec_init(&m->layers);
  syntaqlite_vec_init(&m->traceback_buf);
  syntaqlite_vec_init(&m->node_expanded_buf);
}

void synq_macro_state_free(SynqMacroState* m, SyntaqliteMemMethods mem) {
  syntaqlite_vec_free(&m->expand_buf, mem);
  syntaqlite_vec_free(&m->body_buf, mem);
  synq_layers_free_owned(&m->layers, mem);
  syntaqlite_vec_free(&m->layers, mem);
  syntaqlite_vec_free(&m->traceback_buf, mem);
  syntaqlite_vec_free(&m->node_expanded_buf, mem);
}

void synq_layers_free_owned(SynqExpansionLayerVec* layers,
                            SyntaqliteMemMethods mem) {
  for (uint32_t i = 1; i < syntaqlite_vec_len(layers); i++) {
    SynqExpansionLayer* lyr = &layers->data[i];
    if (lyr->expansion_data)
      mem.xFree((void*)lyr->expansion_data);
    if (lyr->arg_segments)
      mem.xFree(lyr->arg_segments);
    if (lyr->args)
      mem.xFree(lyr->args);
  }
}

void synq_layers_push_sentinel(SynqExpansionLayerVec* layers,
                               const char* source,
                               uint32_t source_len,
                               SyntaqliteMemMethods mem) {
  SynqExpansionLayer sentinel = {
      .call_offset = 0,
      .call_length = 0,
      .expansion_data = source,
      .expansion_len = source_len,
      .parent_layer_id = 0,
  };
  syntaqlite_vec_push(layers, sentinel, mem);
}

// ---------------------------------------------------------------------------
// Macro expansion result (called from inside the lookup callback)
// ---------------------------------------------------------------------------

// Internal: free previous layer data and arg segments.
static void layer_free_data(SyntaqliteParser* p, SynqExpansionLayer* lyr) {
  if (lyr->expansion_data)
    p->mem.xFree((void*)lyr->expansion_data);
  if (lyr->arg_segments)
    p->mem.xFree(lyr->arg_segments);
  if (lyr->args)
    p->mem.xFree(lyr->args);
  lyr->expansion_data = NULL;
  lyr->expansion_len = 0;
  lyr->arg_segments = NULL;
  lyr->arg_segment_count = 0;
  lyr->args = NULL;
  lyr->arg_count = 0;
  lyr->is_fallback = 0;
}

SYNTAQLITE_API void syntaqlite_macro_expansion_set_result(SyntaqliteParser* p,
                                                          const char* body,
                                                          uint32_t body_len,
                                                          uint32_t def_line,
                                                          uint32_t def_col) {
  SynqExpansionLayer* lyr = &p->macro.layers.data[p->macro.pending_layer];
  layer_free_data(p, lyr);
  char* d = p->mem.xMalloc(body_len + 1);
  memcpy(d, body, body_len);
  d[body_len] = '\0';
  lyr->expansion_data = d;
  lyr->expansion_len = body_len;
  lyr->def_line = def_line;
  lyr->def_col = def_col;
}

SYNTAQLITE_API void syntaqlite_macro_expansion_set_result_with_arg_map(
    SyntaqliteParser* p,
    const char* body,
    uint32_t body_len,
    uint32_t def_line,
    uint32_t def_col,
    const SyntaqliteArgMapping* mappings,
    uint32_t mapping_count) {
  syntaqlite_macro_expansion_set_result(p, body, body_len, def_line, def_col);

  if (mapping_count == 0)
    return;

  // Build resolved SynqArgSegment array from the caller's mappings.
  SynqExpansionLayer* lyr = &p->macro.layers.data[p->macro.pending_layer];
  const SyntaqliteToken* args = p->macro.expansion_args;
  uint32_t arg_count = p->macro.expansion_arg_count;
  uint32_t origin_layer_id = lyr->parent_layer_id;
  // Origin-layer base: stmt_source for layer 0 (so origin_offset is
  // statement-relative), expansion buffer otherwise.
  const char* origin_base =
      origin_layer_id == 0
          ? p->stmt_source
          : p->macro.layers.data[origin_layer_id].expansion_data;

  SynqArgSegment* segs = p->mem.xMalloc(mapping_count * sizeof(SynqArgSegment));
  uint32_t seg_count = 0;

  for (uint32_t i = 0; i < mapping_count; i++) {
    uint32_t ai = mappings[i].arg_index;
    if (ai >= arg_count)
      continue;
    uint32_t alen = args[ai].length;
    if (alen == 0)
      continue;
    segs[seg_count++] = (SynqArgSegment){
        .sub_offset = mappings[i].body_offset,
        .sub_length = alen,
        .origin_layer_id = origin_layer_id,
        .origin_offset = (uint32_t)(args[ai].text - origin_base),
        .origin_length = alen,
    };
  }

  lyr->arg_segments = segs;
  lyr->arg_segment_count = seg_count;
}

// Forward declaration — mutual recursion with expand_and_feed.
// (canonical declaration in parser_internal.h)

// Tokenize `buf` and feed each token to Lemon.
// `depth` is the current expansion nesting (for recursion limit).
// Returns: 0 = ok, 1 = statement boundary, -1 = error.
static int expand_and_feed(SyntaqliteParser* p,
                           const char* buf,
                           uint32_t buf_len,
                           uint32_t depth) {
  if (depth >= SYNQ_MAX_MACRO_DEPTH) {
    snprintf(p->error_msg, sizeof(p->error_msg),
             "macro expansion depth limit exceeded (%d)", SYNQ_MAX_MACRO_DEPTH);
    p->had_error = 1;
    return -1;
  }

  // Temporarily swap ctx.source so Lemon action offset computations are
  // relative to the expansion buffer.
  const char* saved_source = p->ctx.source;
  p->ctx.source = buf;

  const unsigned char* z = (const unsigned char*)buf;
  uint32_t pos = 0;

  while (pos < buf_len) {
    uint32_t ttype = 0;
    int64_t tlen = synq_macro_skip(p, z, buf_len, &pos, &ttype);
    if (tlen <= 0)
      break;

    // Check for nested macro call: ID followed by TK_BANG, mirroring the
    // main parse loop's next_token() behaviour and skipping any whitespace
    // or comments between the two (issue #130). Suppressed inside macro
    // definition bodies — those should be tokenized verbatim.
    uint32_t bang_pos = pos + (uint32_t)tlen;
    uint32_t la_type = 0;
    if (ttype == SYNTAQLITE_TK_ID && p->ctx.in_macro_def_body == 0)
      synq_macro_skip(p, z, buf_len, &bang_pos, &la_type);
    if (ttype == SYNTAQLITE_TK_ID && la_type == SYNTAQLITE_TK_BANG &&
        p->ctx.in_macro_def_body == 0) {
      uint32_t nested_end = 0;
      int erc = synq_parser_expand_and_feed_macro(p, buf, buf_len, pos,
                                                  (uint32_t)tlen, bang_pos,
                                                  depth + 1, &nested_end);
      if (erc == 0) {
        pos = nested_end;
        continue;
      }
      // erc == -1: not a macro or error — feed ID normally below.
      if (p->had_error) {
        p->ctx.source = saved_source;
        return -1;
      }
    }

    // Feed the token through the unified shift path: it pushes to
    // `p->tokens` with a real `token_idx` (tagged with the current
    // expansion layer) and then feeds Lemon.  `pos` is the token's
    // offset within the expansion buffer; `p->ctx.layer_id` was set
    // to the current expansion's index before expand_and_feed was
    // called.  For layer-N the shift function leaves `p->ctx.error`
    // intact so we can attach a macro-specific error message here.
    int frc = synq_parser_shift_token(p, ttype, buf + pos, (uint32_t)tlen,
                                      (uint32_t)pos);

    if (p->ctx.error) {
      p->had_error = 1;
      if (p->error_msg[0] == '\0') {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "syntax error in macro expansion near '%.*s'", (int)tlen,
                 buf + pos);
      }
      p->ctx.error = 0;
    }

    if (frc == 1 || p->ctx.stmt_completed) {
      p->ctx.stmt_completed = 0;
      p->ctx.source = saved_source;
      return 1;
    }

    pos += (uint32_t)tlen;
  }

  p->ctx.source = saved_source;
  return 0;
}

// Expand a macro call and feed the expanded tokens into the parser.
//
// Combines lookup-callback invocation, layer creation, and token feeding
// into a single operation.  The layer is pushed *before* the callback so
// that set_result / expand_and_set_result can write directly into it.
//
// Returns 0 on success, -1 if not a registered macro or on error.
// On success, *out_end_offset is set to the byte past the closing paren.
int synq_parser_expand_and_feed_macro(SyntaqliteParser* p,
                                      const char* buf,
                                      uint32_t buf_len,
                                      uint32_t id_offset,
                                      uint32_t id_len,
                                      uint32_t bang_offset,
                                      uint32_t depth,
                                      uint32_t* out_end_offset) {
  if (!p->macro.lookup_fn)
    return -1;

  // Recursion check.  Walks the chain of lexical wrappers (the macros
  // whose authored bodies contain the call site we're about to expand)
  // looking for a match against the callee's name.
  //
  // The chain isn't quite the parent-layer chain: a call that came in
  // through a `$param` substitution was authored by the substituting
  // layer's caller, not by the substituting layer itself, so the
  // substituting layer doesn't belong on the chain.  Two filters
  // capture that:
  //
  //  1. If `id_offset` falls inside one of the current layer's
  //     `arg_segments`, the call we're about to expand was authored
  //     by the current layer's caller — start the walk one layer up.
  //
  //  2. When walking, stop at any layer whose `body_call_offset` is
  //     ARG_INTERNAL: its own call came through a `$param` substitution
  //     too, and anything further up doesn't lexically author the call
  //     site either.
  {
    uint32_t walk = p->ctx.layer_id;
    if (walk > 0) {
      const SynqExpansionLayer* cur = &p->macro.layers.data[walk];
      for (uint32_t i = 0; i < cur->arg_segment_count; i++) {
        const SynqArgSegment* seg = &cur->arg_segments[i];
        if (id_offset >= seg->sub_offset &&
            id_offset < seg->sub_offset + seg->sub_length) {
          walk = cur->parent_layer_id;
          break;
        }
      }
    }
    while (walk > 0) {
      const SynqExpansionLayer* lyr = &p->macro.layers.data[walk];
      if (synq_name_eq_ci(lyr->name, lyr->name_len, buf + id_offset, id_len)) {
        snprintf(p->error_msg, sizeof(p->error_msg),
                 "recursive macro expansion: '%.*s'", (int)id_len,
                 buf + id_offset);
        p->had_error = 1;
        return -1;
      }
      if (lyr->body_call_offset == SYNTAQLITE_MACRO_BODY_CALL_ARG_INTERNAL)
        break;
      walk = lyr->parent_layer_id;
    }
  }

  // Scan args.
  SynqMacroArg args[64];
  uint32_t end_offset = 0;
  uint32_t arg_count = synq_parser_scan_macro_args(p, buf, buf_len, bang_offset,
                                                   args, 64, &end_offset);

  SyntaqliteToken token_args[64];
  uint32_t token_arg_count = arg_count < 64 ? arg_count : 64;
  for (uint32_t i = 0; i < token_arg_count; i++) {
    token_args[i].text = buf + args[i].offset;
    token_args[i].length = args[i].length;
    token_args[i].type = 0;
  }
  // Push the expansion layer *before* the callback so set_result /
  // expand_and_set_result can write directly into it.
  uint32_t call_length = end_offset - id_offset;
  begin_macro_expansion(p, id_offset, call_length, buf + id_offset, id_len);

  uint32_t new_layer_idx = syntaqlite_vec_len(&p->macro.layers) - 1;

  p->macro.pending_layer = new_layer_idx;
  p->macro.expansion_args = token_args;
  p->macro.expansion_arg_count = token_arg_count;

  int rc = p->macro.lookup_fn(p->macro.lookup_user_data, p, buf + id_offset,
                              id_len, token_args, token_arg_count);
  p->macro.expansion_args = NULL;
  p->macro.expansion_arg_count = 0;

  if (rc == -1 || rc == -2) {
    SynqExpansionLayer* lyr = &p->macro.layers.data[new_layer_idx];
    if (lyr->expansion_data)
      p->mem.xFree((void*)lyr->expansion_data);
    if (lyr->arg_segments)
      p->mem.xFree(lyr->arg_segments);
    if (lyr->args)
      p->mem.xFree(lyr->args);
    p->macro.layers.count--;
    p->macro.depth--;
    if (rc == -2)
      p->had_error = 1;
    return -1;
  }

  // The callback wrote expansion_data/len/def_line/def_col onto the
  // layer.  Persist the call-site arg spans now (after the callback,
  // since `set_result` calls `layer_free_data` which would wipe them
  // otherwise) so downstream consumers — formatter, spans API,
  // traceback — can read them without re-running scan_macro_args.
  // Offsets in `args[]` are buf-relative; rebase top-level layers to
  // statement-relative so they match how `begin_macro_expansion`
  // stored `call_offset`.
  SynqExpansionLayer* lyr = &p->macro.layers.data[new_layer_idx];
  if (token_arg_count > 0) {
    SynqMacroArg* heap = p->mem.xMalloc(token_arg_count * sizeof(SynqMacroArg));
    uint32_t shift = lyr->parent_layer_id == 0 ? p->stmt_start_offset : 0;
    for (uint32_t i = 0; i < token_arg_count; i++) {
      heap[i].offset = args[i].offset - shift;
      heap[i].length = args[i].length;
    }
    lyr->args = heap;
    lyr->arg_count = token_arg_count;
  }
  const char* data = lyr->expansion_data;
  uint32_t data_len = lyr->expansion_len;

  p->ctx.layer_id = new_layer_idx;

  // Feed expanded tokens (may trigger nested macro expansions).
  int frc = expand_and_feed(p, data, data_len, depth);

  synq_end_macro(p);

  if (frc < 0)
    return -1;

  *out_end_offset = end_offset;
  return 0;
}

// ---------------------------------------------------------------------------
// Macro region tracking (internal helper + public begin/end)
// ---------------------------------------------------------------------------

// Internal: push a new expansion layer.
// expansion_data, def_line, def_col are left zeroed — the callback fills
// them via set_result / expand_and_set_result.
static void begin_macro_expansion(SyntaqliteParser* p,
                                  uint32_t call_offset,
                                  uint32_t call_length,
                                  const char* name,
                                  uint32_t name_len) {
  // Top-level call_offset is absolute; rebase to statement-relative
  // so layer call_offset / macro_root_start / body_call_offset match
  // every other per-statement offset.
  if (p->ctx.layer_id == 0) {
    call_offset -= p->stmt_start_offset;
  }

  // Stash the outermost macro call-site range so per-node extent
  // tracking can attribute tokens from inside this (or any nested)
  // expansion back to the authored source.
  if (p->ctx.layer_id == 0) {
    p->ctx.macro_root_start = call_offset;
    p->ctx.macro_root_end = call_offset + call_length;
    if (p->ctx.macro_root_layer == 0) {
      for (uint32_t i = 0; i < p->ctx.lemon_depth; i++)
        syntaqlite_vec_push(&p->ctx.straddle_stack, SYNQ_STRADDLE_NEUTRAL,
                            p->mem);
    }
    p->ctx.macro_root_layer = syntaqlite_vec_len(&p->macro.layers);
  }

  // Compute position of this call in the parent's *authored* body by
  // inverting the length shifts introduced by the parent's $param
  // substitutions.  For top-level calls the parent is the source layer
  // (no arg segments), so the shifts stay zero and body_call_offset /
  // body_call_length equal call_offset / call_length.
  //
  // A segment's relationship to the call range determines its effect:
  //   * fully before call → its length delta shifts body_call_offset
  //   * fully after call  → no effect
  //   * seg contains call (incl. equal bounds) → arg-internal: the call
  //     was tokenized from this arg's substituted text
  //   * seg strictly inside call → its length delta shrinks body_call_length
  //   * partial overlap → arg-internal
  //
  // `body_shift` is signed: positive when the substitution is longer
  // than the `$param` token (body grows), negative when shorter (body
  // shrinks).  Either way, `body_coord = sub_coord - accumulated_shift`.
  //
  // The "contains" check must run before "strictly inside" so the
  // equal-bounds case (common for `m!(arg)` where arg is itself a
  // macro call) is classified as arg-internal rather than inside.
  const SynqExpansionLayer* parent = &p->macro.layers.data[p->ctx.layer_id];
  uint32_t call_end = call_offset + call_length;
  int64_t prefix_shift = 0;
  int64_t inner_shift = 0;
  int arg_internal = 0;
  for (uint32_t i = 0; i < parent->arg_segment_count; i++) {
    const SynqArgSegment* seg = &parent->arg_segments[i];
    uint32_t seg_end = seg->sub_offset + seg->sub_length;
    int64_t body_shift = (int64_t)seg->sub_length - (int64_t)seg->body_length;
    if (seg_end <= call_offset) {
      prefix_shift += body_shift;
    } else if (seg->sub_offset >= call_end) {
      // Fully after the call — no effect.
    } else if (seg->sub_offset <= call_offset && seg_end >= call_end) {
      arg_internal = 1;
      break;
    } else if (seg->sub_offset >= call_offset && seg_end <= call_end) {
      inner_shift += body_shift;
    } else {
      arg_internal = 1;
      break;
    }
  }
  uint32_t body_call_offset =
      arg_internal ? SYNTAQLITE_MACRO_BODY_CALL_ARG_INTERNAL
                   : (uint32_t)((int64_t)call_offset - prefix_shift);
  uint32_t body_call_length =
      arg_internal ? SYNTAQLITE_MACRO_BODY_CALL_ARG_INTERNAL
                   : (uint32_t)((int64_t)call_length - inner_shift);

  SynqExpansionLayer layer = {
      .call_offset = call_offset,
      .call_length = call_length,
      .name = name,
      .name_len = name_len,
      .body_call_offset = body_call_offset,
      .body_call_length = body_call_length,
      .parent_layer_id = p->ctx.layer_id,
  };
  syntaqlite_vec_push(&p->macro.layers, layer, p->mem);
  p->macro.depth++;
}

static void synq_end_macro(SyntaqliteParser* p) {
  if (p->macro.depth > 0) {
    p->macro.depth--;
    // Restore layer_id to parent. If we're back to depth 0, that's layer 0
    // (source). Otherwise, find the parent from the current layer.
    if (p->macro.depth == 0) {
      p->ctx.layer_id = 0;
    } else {
      // Walk back to find the still-active parent layer.
      uint32_t cur = p->ctx.layer_id;
      if (cur > 0 && cur < syntaqlite_vec_len(&p->macro.layers)) {
        p->ctx.layer_id = p->macro.layers.data[cur].parent_layer_id;
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Top-level macro dispatch during parsing
// ---------------------------------------------------------------------------

// Try to expand a Rust-style macro call: ID!(args).
// Requires macro_style == RUST and a matching lookup callback (or fallback
// mode). Returns 0 if consumed, -1 if not a macro call, 1 if statement
// boundary.
SYNQ_NOINLINE
int synq_parser_try_macro_call(SyntaqliteParser* p,
                               uint32_t id_offset,
                               uint32_t id_len,
                               uint32_t bang_offset) {
  const unsigned char* z = (const unsigned char*)p->source;
  if (z[bang_offset] != '!')
    return -1;
  if (p->dialect.tmpl->macro_style != SYNQ_MACRO_STYLE_RUST &&
      !p->macro.macro_fallback)
    return -1;
  // Don't expand macros while parsing a macro definition body — the body
  // should be captured verbatim, with nested macro calls preserved as text.
  if (p->ctx.in_macro_def_body > 0)
    return -1;

  if (p->macro.lookup_fn) {
    uint32_t end_off = 0;
    int erc = synq_parser_expand_and_feed_macro(p, p->source, p->source_len,
                                                id_offset, id_len, bang_offset,
                                                1, &end_off);
    if (erc == 0) {
      p->offset = end_off;
      return 0;
    }
    // Not found or error — if had_error was set, propagate.
    if (p->had_error)
      return -1;

    // Lookup callback is registered but the macro was not found.
    // This is a hard error — the user likely misspelled the macro name
    // or forgot to define it.
    snprintf(p->error_msg, sizeof(p->error_msg), "unknown macro '%.*s'",
             (int)id_len, (const char*)z + id_offset);
    p->had_error = 1;
    return -1;
  }

  // No callback — fall through to TK_ID fallback.
  // (We already checked macro_style/macro_fallback at the top.)

  // Scan balanced parens to find the end of name!(args) and capture
  // the top-level arg spans so downstream consumers (notably the
  // formatter's structured-arg pass) don't need to retokenize the
  // call body.  64 is well above any realistic macro arity; calls
  // exceeding that are handled correctly for `end_offset` but have
  // their arg spans dropped (still scanned, just not recorded).
  enum { SYNQ_FALLBACK_ARG_STACK_CAP = 64 };
  uint32_t end_offset = 0;
  SynqMacroArg args_stack[SYNQ_FALLBACK_ARG_STACK_CAP];
  uint32_t arg_count = synq_parser_scan_macro_args(
      p, p->source, p->source_len, bang_offset, args_stack,
      SYNQ_FALLBACK_ARG_STACK_CAP, &end_offset);
  if (end_offset == 0)
    return -1;  // Unbalanced parens — still an error.

  uint32_t call_length = end_offset - id_offset;

  // Record macro region so formatter emits verbatim (no expansion data).
  // Pass the macro name (a source slice) so downstream consumers can
  // read it from the rewrite directly without reparsing the call text.
  begin_macro_expansion(p, id_offset, call_length, (const char*)z + id_offset,
                        id_len);
  p->ctx.layer_id = syntaqlite_vec_len(&p->macro.layers) - 1;

  // Attach captured arg spans to the fresh layer and flag it as a
  // fallback.  scan_macro_args returns source-absolute offsets;
  // begin_macro_expansion rebases top-level call_offset to
  // statement-relative, so apply the same shift to the arg spans.
  SynqExpansionLayer* lyr = &p->macro.layers.data[p->ctx.layer_id];
  lyr->is_fallback = 1;
  if (arg_count > 0 && arg_count <= SYNQ_FALLBACK_ARG_STACK_CAP) {
    SynqMacroArg* heap = p->mem.xMalloc(arg_count * sizeof(SynqMacroArg));
    uint32_t shift = lyr->parent_layer_id == 0 ? p->stmt_start_offset : 0;
    for (uint32_t i = 0; i < arg_count; i++) {
      heap[i].offset = args_stack[i].offset - shift;
      heap[i].length = args_stack[i].length;
    }
    lyr->args = heap;
    lyr->arg_count = arg_count;
  }

  synq_end_macro(p);

  // Feed the whole name!(args) span as a single TK_ID to Lemon.  A
  // TK_ID shift mid-statement cannot complete a statement, so the
  // shift's return value is always 0 and we don't need the main-loop's
  // boundary filter here.
  uint32_t layer_offset = id_offset - p->stmt_start_offset;
  synq_parser_shift_token(p, SYNTAQLITE_TK_ID, p->source + id_offset,
                          call_length, layer_offset);
  p->offset = end_offset;
  return 0;
}

// ---------------------------------------------------------------------------
// Macro lookup callback API
// ---------------------------------------------------------------------------

SYNTAQLITE_API int32_t
syntaqlite_parser_set_macro_lookup(SyntaqliteParser* p,
                                   SyntaqliteMacroLookupFn fn,
                                   void* user_data) {
  p->macro.lookup_fn = fn;
  p->macro.lookup_user_data = user_data;
  return SYNTAQLITE_OK;
}

// ---------------------------------------------------------------------------
// Template expansion helper
// ---------------------------------------------------------------------------

SYNTAQLITE_API int syntaqlite_macro_expansion_expand_and_set_result(
    SyntaqliteParser* p,
    const char* body,
    uint32_t body_len,
    const char* const* param_names,
    const uint32_t* param_name_lens,
    uint32_t param_count,
    uint32_t flags) {
  const SyntaqliteToken* args = p->macro.expansion_args;
  uint32_t arg_count = p->macro.expansion_arg_count;

  if (param_count > 0 && arg_count != param_count)
    return -1;

  // Reuse the parser's scratch vec — reset count but keep the allocation.
  p->macro.expand_buf.count = 0;

  // Stage the body into a NUL-terminated buffer before tokenizing.
  // The tokenizer reads until NUL, but callers may pass non-NUL-terminated
  // buffers (e.g. Rust &str), so we must ensure the NUL sentinel exists.
  p->macro.body_buf.count = 0;
  syntaqlite_vec_push_n(&p->macro.body_buf, (const uint8_t*)body, body_len,
                        p->mem);
  syntaqlite_vec_push(&p->macro.body_buf, 0, p->mem);

  // Collect arg mappings on the stack (max 64 params).  We extend the
  // public SyntaqliteArgMapping with the authored-body position of the
  // $param token (pre-substitution) so downstream tracebacks can anchor
  // substitutions back to the macro definition.
  struct Mapping {
    uint32_t sub_offset;      // Offset in expansion buffer.
    uint32_t arg_index;       // Index into callback args.
    uint32_t body_token_off;  // Offset of $param token in authored body.
    uint32_t body_token_len;  // Length of $param token in authored body.
  } mappings[64];
  uint32_t mapping_count = 0;

  const unsigned char* z = (const unsigned char*)p->macro.body_buf.data;
  const char* zbody = (const char*)p->macro.body_buf.data;
  uint32_t pos = 0;
  while (pos < body_len) {
    uint32_t ttype = 0;
    int64_t tlen =
        SynqSqliteGetTokenVersionWrapped(&p->dialect, 0, z + pos, &ttype);
    if (tlen <= 0)
      break;

    if (ttype == SYNTAQLITE_TK_VARIABLE && zbody[pos] == '$' && tlen > 1) {
      const char* pname = zbody + pos + 1;
      uint32_t pname_len = (uint32_t)tlen - 1;

      int found = -1;
      for (uint32_t pi = 0; pi < param_count; pi++) {
        if (param_name_lens[pi] == pname_len &&
            memcmp(param_names[pi], pname, pname_len) == 0) {
          found = (int)pi;
          break;
        }
      }

      if (found < 0) {
        if (flags & SYNTAQLITE_EXPAND_PASSTHROUGH_UNKNOWN) {
          // Copy the unknown $param verbatim into the expansion buffer.
          syntaqlite_vec_push_n(&p->macro.expand_buf,
                                (const uint8_t*)(zbody + pos), (uint32_t)tlen,
                                p->mem);
          pos += (uint32_t)tlen;
          continue;
        }
        return -1;
      }

      if ((uint32_t)found < arg_count && args[found].length > 0) {
        if (mapping_count < 64) {
          mappings[mapping_count].sub_offset = p->macro.expand_buf.count;
          mappings[mapping_count].arg_index = (uint32_t)found;
          mappings[mapping_count].body_token_off = pos;
          mappings[mapping_count].body_token_len = (uint32_t)tlen;
          mapping_count++;
        }
        syntaqlite_vec_push_n(&p->macro.expand_buf,
                              (const uint8_t*)args[found].text,
                              args[found].length, p->mem);
      }
    } else {
      syntaqlite_vec_push_n(&p->macro.expand_buf, (const uint8_t*)(zbody + pos),
                            (uint32_t)tlen, p->mem);
    }

    pos += (uint32_t)tlen;
  }

  // Steal the scratch vec's buffer directly into the layer (no copy).
  // Null-terminate for safety.
  syntaqlite_vec_push(&p->macro.expand_buf, 0, p->mem);
  SynqExpansionLayer* lyr = &p->macro.layers.data[p->macro.pending_layer];
  layer_free_data(p, lyr);
  lyr->expansion_data = (const char*)p->macro.expand_buf.data;
  lyr->expansion_len = p->macro.expand_buf.count - 1;  // exclude NUL
  lyr->def_line = 0;
  lyr->def_col = 0;
  // Detach buffer from vec so it won't be freed when vec is reused.
  p->macro.expand_buf.data = NULL;
  p->macro.expand_buf.count = 0;
  p->macro.expand_buf.capacity = 0;

  // Build arg segments from the mappings we collected.
  if (mapping_count > 0) {
    uint32_t origin_layer_id = lyr->parent_layer_id;
    const char* origin_base =
        origin_layer_id == 0
            ? p->stmt_source
            : p->macro.layers.data[origin_layer_id].expansion_data;

    SynqArgSegment* segs =
        p->mem.xMalloc(mapping_count * sizeof(SynqArgSegment));
    for (uint32_t i = 0; i < mapping_count; i++) {
      uint32_t ai = mappings[i].arg_index;
      segs[i] = (SynqArgSegment){
          .body_offset = mappings[i].body_token_off,
          .body_length = mappings[i].body_token_len,
          .sub_offset = mappings[i].sub_offset,
          .sub_length = args[ai].length,
          .origin_layer_id = origin_layer_id,
          .origin_offset = (uint32_t)(args[ai].text - origin_base),
          .origin_length = args[ai].length,
      };
    }
    lyr->arg_segments = segs;
    lyr->arg_segment_count = mapping_count;
  }

  return SYNTAQLITE_OK;
}

#endif  // !SYNTAQLITE_OMIT_MACROS
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/parser_macros.c ======== */

/* ======== begin: csrc/parser_spans.c ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Span resolution and traceback.
//
// Navigates the expansion layer tree to resolve AST span coordinates
// back to authored source positions.  Compiled out when
// SYNTAQLITE_OMIT_MACROS is defined — without macro expansion, span_text
// and traceback are stubbed in parser.c.

#ifndef SYNTAQLITE_OMIT_MACROS

#include <string.h>



// ---------------------------------------------------------------------------
// Span accessors: span_text, span_expanded_text
// ---------------------------------------------------------------------------

// Try to drill an (offset, length) span through a layer's arg segments.
// If the span lies fully inside a substituted arg, updates *off, *len,
// *layer to the arg's origin and returns 1.  Otherwise returns 0.
static int try_arg_drill(const SynqExpansionLayer* lyr,
                         uint32_t* off,
                         uint32_t* len,
                         uint32_t* layer) {
  for (uint32_t i = 0; i < lyr->arg_segment_count; i++) {
    const SynqArgSegment* seg = &lyr->arg_segments[i];
    if (*off >= seg->sub_offset &&
        *off + *len <= seg->sub_offset + seg->sub_length) {
      uint32_t delta = *off - seg->sub_offset;
      *off = seg->origin_offset + delta;
      *layer = seg->origin_layer_id;
      return 1;
    }
  }
  return 0;
}

// Walk the layer chain to resolve (layer, offset, length) to an authored
// byte range in the source.  At each layer, collapse to the layer's call
// site in the parent and continue walking.
//
// Bounded by SYNQ_MAX_MACRO_DEPTH; typical case is one or two iterations.
static void span_walk_to_source(SyntaqliteParser* p,
                                uint8_t layer_id,
                                uint32_t offset,
                                uint32_t length,
                                uint32_t* out_offset,
                                uint32_t* out_length) {
  uint32_t off = offset;
  uint32_t len = length;
  uint32_t layer = layer_id;
  uint32_t layers_count = syntaqlite_vec_len(&p->macro.layers);
  // Each iteration either drills into an arg origin layer or moves up
  // to a parent layer; cap defensively at twice the max depth.
  for (uint32_t step = 0; step < 2 * (SYNQ_MAX_MACRO_DEPTH + 1); step++) {
    if (layer == 0 || layer >= layers_count) {
      break;
    }
    const SynqExpansionLayer* cur = &p->macro.layers.data[layer];

    // Arg-segment drill: if the span lies fully inside a substituted arg,
    // the authored bytes live in the segment's origin layer, not via the
    // layer's call site.
    if (try_arg_drill(cur, &off, &len, &layer))
      continue;

    // Collapse to the call site in the parent layer.
    off = cur->call_offset;
    len = cur->call_length;
    layer = cur->parent_layer_id;
  }
  *out_offset = off;
  *out_length = len;
}

SYNTAQLITE_API const char* syntaqlite_parser_span_expanded_text(
    SyntaqliteParser* p,
    const SyntaqliteTextSpan* span,
    uint32_t* out_len) {
  if (!span || span->length == 0) {
    *out_len = 0;
    return NULL;
  }
  uint32_t layer = span->_layer_id;
  if (layer >= syntaqlite_vec_len(&p->macro.layers)) {
    *out_len = 0;
    return NULL;
  }
  const SynqExpansionLayer* lyr = &p->macro.layers.data[layer];
  if (!lyr->expansion_data ||
      span->offset + span->length > lyr->expansion_len) {
    *out_len = 0;
    return NULL;
  }
  *out_len = span->length;
  return lyr->expansion_data + span->offset;
}

SYNTAQLITE_API const char* syntaqlite_parser_span_text(
    SyntaqliteParser* p,
    const SyntaqliteTextSpan* span,
    uint32_t* out_len,
    uint32_t* out_offset) {
  if (out_offset)
    *out_offset = 0;
  if (!span) {
    *out_len = 0;
    return NULL;
  }
  // A span's offset is positional metadata that is always meaningful for
  // an in-range span, independent of length.  A zero-length span can be
  // either an absent field (zero-initialized `{0,0,0,0}` — offset 0 is
  // the sentinel) or a genuine empty-but-quoted token like `""`
  // (non-zero offset + quote flag set).  Callers distinguish the two via
  // `TextSpan::is_quoted`; our job here is to surface the real offset in
  // both cases rather than collapsing to 0.
  uint32_t stmt_len = p->stmt_end_offset > p->stmt_start_offset
                          ? p->stmt_end_offset - p->stmt_start_offset
                          : 0;
  // Layer-0 spans are already statement-relative.
  if (span->_layer_id == 0) {
    if (span->offset + span->length > stmt_len) {
      *out_len = 0;
      return NULL;
    }
    *out_len = span->length;
    if (out_offset)
      *out_offset = span->offset;
    return p->stmt_source + span->offset;
  }
  // Walk the expansion chain.  call_offsets with parent==layer 0 and
  // origin_offsets with origin_layer_id==0 are statement-relative, so
  // a walk that terminates at layer 0 yields a statement-relative
  // offset; deeper layers yield buffer-local offsets.
  uint32_t off = 0;
  uint32_t len = 0;
  span_walk_to_source(p, span->_layer_id, span->offset, span->length, &off,
                      &len);
  if (off + len > stmt_len) {
    *out_len = 0;
    return NULL;
  }
  *out_len = len;
  if (out_offset)
    *out_offset = off;
  return p->stmt_source + off;
}

// ---------------------------------------------------------------------------
// Traceback
// ---------------------------------------------------------------------------

// Compute 1-based (line, col) for `offset` within `buf[..buf_len]`.  The
// offset is clamped to `buf_len`.
static void compute_line_col(const char* buf,
                             uint32_t buf_len,
                             uint32_t offset,
                             uint32_t* out_line,
                             uint32_t* out_col) {
  if (offset > buf_len)
    offset = buf_len;
  // Walk newline-to-newline with memchr instead of char-by-char so
  // long single-line SQL doesn't become O(offset) per frame.
  uint32_t line = 1;
  uint32_t last_nl_end = 0;  // byte position just past the most recent '\n'
  uint32_t scanned = 0;
  while (scanned < offset) {
    const char* nl =
        (const char*)memchr(buf + scanned, '\n', (size_t)(offset - scanned));
    if (!nl)
      break;
    line++;
    last_nl_end = (uint32_t)(nl - buf) + 1;
    scanned = last_nl_end;
  }
  *out_line = line;
  *out_col = offset - last_nl_end + 1;
}

SYNTAQLITE_API const SyntaqliteTracebackFrame* syntaqlite_parser_traceback(
    SyntaqliteParser* p,
    const SyntaqliteTextSpan* sp,
    uint32_t* out_count) {
  if (out_count)
    *out_count = 0;
  // Clear the scratch buffer from any previous call.  Keeps the
  // allocation so repeat calls reuse the same heap block.
  syntaqlite_vec_clear(&p->macro.traceback_buf);
  if (!sp || sp->length == 0)
    return NULL;

  // Walk the layer chain, emitting one frame per layer into a small
  // on-stack buffer (innermost first).  Then reverse into the parser's
  // owned vec so the caller sees outermost first.
  SyntaqliteTracebackFrame tmp[SYNQ_MAX_MACRO_DEPTH + 2];
  uint32_t count = 0;
  uint32_t off = sp->offset;
  uint32_t len = sp->length;
  uint32_t layer_id = sp->_layer_id;
  uint32_t layers_count = syntaqlite_vec_len(&p->macro.layers);

  for (uint32_t step = 0; step < 2 * (SYNQ_MAX_MACRO_DEPTH + 1) &&
                          count < SYNQ_MAX_MACRO_DEPTH + 2;
       step++) {
    if (layer_id >= layers_count)
      break;
    const SynqExpansionLayer* lyr = &p->macro.layers.data[layer_id];

    if (layer_id == 0) {
      // Root (sentinel) — emit final frame and terminate.
      SyntaqliteTracebackFrame* f = &tmp[count++];
      f->name = lyr->name;
      f->name_len = lyr->name_len;
      f->snippet = lyr->expansion_data;
      f->snippet_len = lyr->expansion_len;
      f->offset_in_snippet = off;
      f->length_in_snippet = len;
      compute_line_col(lyr->expansion_data, lyr->expansion_len, off, &f->line,
                       &f->col);
      break;
    }

    // Arg-segment drill: if the span lies fully inside a substituted arg,
    // skip this layer's frame and drill to the arg's origin.
    uint32_t tb_layer = layer_id;
    if (try_arg_drill(lyr, &off, &len, &tb_layer)) {
      layer_id = (uint8_t)tb_layer;
      continue;
    }

    // No drill — emit a frame for this expansion layer.
    SyntaqliteTracebackFrame* f = &tmp[count++];
    f->name = lyr->name;
    f->name_len = lyr->name_len;
    f->snippet = lyr->expansion_data;
    f->snippet_len = lyr->expansion_len;
    f->offset_in_snippet = off;
    f->length_in_snippet = len;
    compute_line_col(lyr->expansion_data, lyr->expansion_len, off, &f->line,
                     &f->col);

    // Walk up to parent at this layer's call site.
    off = lyr->call_offset;
    len = lyr->call_length;
    layer_id = lyr->parent_layer_id;
  }

  if (count == 0)
    return NULL;

  // Reverse into the parser's owned buffer so frame[0] is outermost.
  syntaqlite_vec_ensure(&p->macro.traceback_buf, count, p->mem);
  for (uint32_t i = 0; i < count; i++) {
    p->macro.traceback_buf.data[i] = tmp[count - 1 - i];
  }
  p->macro.traceback_buf.count = count;
  if (out_count)
    *out_count = count;
  return p->macro.traceback_buf.data;
}

#endif  // !SYNTAQLITE_OMIT_MACROS
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/parser_spans.c ======== */

/* ======== begin: syntaqlite_dialect/dialect_macros.h ======== */
#ifndef SYNTAQLITE_INTERNAL_DIALECT_MACROS_H
#define SYNTAQLITE_INTERNAL_DIALECT_MACROS_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Compile-time / runtime gating macros for version and cflag checks.
//
// When SYNTAQLITE_SQLITE_VERSION is defined (compile-time pinning), these
// expand to integer constants and the compiler eliminates dead branches.
// When not defined, they check through the runtime config pointer.
//
// Include this header in .c files that perform version/cflag gating;
// do not expose it in public headers.


// True if the target version is older than `ver`.
#ifdef SYNTAQLITE_SQLITE_VERSION
#define SYNQ_VER_LT(env, ver) (SYNTAQLITE_SQLITE_VERSION < (ver))
#else
#define SYNQ_VER_LT(env, ver) ((env)->sqlite_version < (ver))
#endif

// True if cflag at index `idx` is set in the env.
//
// When SYNTAQLITE_SQLITE_CFLAGS is defined (compile-time cflag pinning),
// reads from the synq_pinned_cflags struct built in sqlite_cflags.h from
// individual SYNTAQLITE_CFLAG_* defines. The compiler constant-folds the
// bit extraction and eliminates dead branches.
#ifdef SYNTAQLITE_SQLITE_CFLAGS
#define SYNQ_HAS_CFLAG(env, idx) synq_has_cflag(&synq_pinned_cflags, (idx))
#else
#define SYNQ_HAS_CFLAG(env, idx) synq_has_cflag(&(env)->cflags, (idx))
#endif


#endif  /* SYNTAQLITE_INTERNAL_DIALECT_MACROS_H */
/* ======== end: syntaqlite_dialect/dialect_macros.h ======== */

/* ======== begin: csrc/sqlite_keyword.c ======== */
/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
** @generated by syntaqlite-buildtools — DO NOT EDIT
*/


/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/
const unsigned char sqlite3UpperToLower[] = {
#ifdef SQLITE_ASCII
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, 16, 17,
     18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35,
     36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
     54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 97, 98, 99,100,101,102,103,
    104,105,106,107,108,109,110,111,112,113,114,115,116,117,118,119,120,121,
    122, 91, 92, 93, 94, 95, 96, 97, 98, 99,100,101,102,103,104,105,106,107,
    108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,125,
    126,127,128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,160,161,
    162,163,164,165,166,167,168,169,170,171,172,173,174,175,176,177,178,179,
    180,181,182,183,184,185,186,187,188,189,190,191,192,193,194,195,196,197,
    198,199,200,201,202,203,204,205,206,207,208,209,210,211,212,213,214,215,
    216,217,218,219,220,221,222,223,224,225,226,227,228,229,230,231,232,233,
    234,235,236,237,238,239,240,241,242,243,244,245,246,247,248,249,250,251,
    252,253,254,255,
#endif
#ifdef SQLITE_EBCDIC
      0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15, /* 0x */
     16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, /* 1x */
     32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, /* 2x */
     48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, /* 3x */
     64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, /* 4x */
     80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, /* 5x */
     96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,111, /* 6x */
    112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127, /* 7x */
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143, /* 8x */
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159, /* 9x */
    160,161,162,163,164,165,166,167,168,169,170,171,140,141,142,175, /* Ax */
    176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191, /* Bx */
    192,129,130,131,132,133,134,135,136,137,202,203,204,205,206,207, /* Cx */
    208,145,146,147,148,149,150,151,152,153,218,219,220,221,222,223, /* Dx */
    224,225,162,163,164,165,166,167,168,169,234,235,236,237,238,239, /* Ex */
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255, /* Fx */
#endif
/* All of the upper-to-lower conversion data is above.  The following
** 18 integers are completely unrelated.  They are appended to the
** sqlite3UpperToLower[] array to avoid UBSAN warnings.  Here's what is
** going on:
**
** The SQL comparison operators (<>, =, >, <=, <, and >=) are implemented
** by invoking sqlite3MemCompare(A,B) which compares values A and B and
** returns negative, zero, or positive if A is less then, equal to, or
** greater than B, respectively.  Then the true false results is found by
** consulting sqlite3aLTb[opcode], sqlite3aEQb[opcode], or 
** sqlite3aGTb[opcode] depending on whether the result of compare(A,B)
** is negative, zero, or positive, where opcode is the specific opcode.
** The only works because the comparison opcodes are consecutive and in
** this order: NE EQ GT LE LT GE.  Various assert()s throughout the code
** ensure that is the case.
**
** These elements must be appended to another array.  Otherwise the
** index (here shown as [256-OP_Ne]) would be out-of-bounds and thus
** be undefined behavior.  That's goofy, but the C-standards people thought
** it was a good idea, so here we are.
*/
/* NE  EQ  GT  LE  LT  GE  */
   1,  0,  0,  1,  1,  0,  /* aLTb[]: Use when compare(A,B) less than zero */
   0,  1,  0,  1,  0,  1,  /* aEQb[]: Use when compare(A,B) equals zero */
   1,  0,  1,  0,  0,  1   /* aGTb[]: Use when compare(A,B) greater than zero*/
};

/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/
#ifdef SQLITE_ASCII
# define charMap(X) sqlite3UpperToLower[(unsigned char)X]
#endif
#ifdef SQLITE_EBCDIC
# define charMap(X) ebcdicToAscii[(unsigned char)X]
const unsigned char ebcdicToAscii[] = {
/* 0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 0x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 1x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 2x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 3x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 4x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 5x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 95,  0,  0,  /* 6x */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* 7x */
   0, 97, 98, 99,100,101,102,103,104,105,  0,  0,  0,  0,  0,  0,  /* 8x */
   0,106,107,108,109,110,111,112,113,114,  0,  0,  0,  0,  0,  0,  /* 9x */
   0,  0,115,116,117,118,119,120,121,122,  0,  0,  0,  0,  0,  0,  /* Ax */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* Bx */
   0, 97, 98, 99,100,101,102,103,104,105,  0,  0,  0,  0,  0,  0,  /* Cx */
   0,106,107,108,109,110,111,112,113,114,  0,  0,  0,  0,  0,  0,  /* Dx */
   0,  0,115,116,117,118,119,120,121,122,  0,  0,  0,  0,  0,  0,  /* Ex */
   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  /* Fx */
};
#endif

/***** This file contains automatically generated code ******
**
** The code in this file has been automatically generated by
**
**   sqlite/tool/mkkeywordhash.c
**
** The code in this file implements a function that determines whether
** or not a given identifier is really an SQL keyword.  The same thing
** might be implemented more directly using a hand-written hash table.
** But by using this automatically generated code, the size of the code
** is substantially reduced.  This is important for embedded applications
** on platforms with limited memory.
*/
/* Hash score: 270 */
/* synq_perfetto_zKWText[] encodes 1127 bytes of keyword text in 740 bytes */
/*   REINDEXEDESCAPERFETTOFFSETABLEFTHENDATABASELECTIESAVEPOINT         */
/*   ERVALUESBEFOREIGNOREGEXPLAINCLUDEFERRABLEACHECKEYISNULLS           */
/*   NOTNULLIKELSEXCLUDELEGATESCONSTRAINTERSECTIONATURALTERAISE         */
/*   XCEPTRANSACTIONOTHINGENERATEDELETEMPORARYMACROSSUNIQUERYWINDOW     */
/*   NWITHOUTERANGEXCLUSIVEXISTSACCUMULATEATTACHAVINGLOBEGINSTEADD      */
/*   ETACHBETWEENBITORDEREFERENCESCASCADEFAULTREECASECOLLATECREATE      */
/*   CURRENT_DATEIMMEDIATEJOINNERELEASEMATCHMODULEPLANALYZEPRAGMA       */
/*   TERIALIZEDEFERREDISTINCTRIGGERECURSIVEUPDATEVIRTUALWAYSWHEN        */
/*   WHERENAMEWITHINSERTABORTAFTEREPLACEANDROPARTITIONAUTOINCREMENT     */
/*   CASTCOLUMNCOMMITCONFLICTCURRENT_TIMESTAMPRECEDINGROUPSFAILAST      */
/*   FILTERESTRICTFIRSTFOLLOWINGFROMFULLIMITFUNCTIONIFINTOTHERSOVER     */
/*   ETURNINGRETURNSRIGHTROLLBACKROWSUNBOUNDEDUNIONUSINGVACUUMVIEW      */
/*   BYINITIALLYPRIMARY                                                 */
const char synq_perfetto_zKWText[739] = {
  'R','E','I','N','D','E','X','E','D','E','S','C','A','P','E','R','F','E',
  'T','T','O','F','F','S','E','T','A','B','L','E','F','T','H','E','N','D',
  'A','T','A','B','A','S','E','L','E','C','T','I','E','S','A','V','E','P',
  'O','I','N','T','E','R','V','A','L','U','E','S','B','E','F','O','R','E',
  'I','G','N','O','R','E','G','E','X','P','L','A','I','N','C','L','U','D',
  'E','F','E','R','R','A','B','L','E','A','C','H','E','C','K','E','Y','I',
  'S','N','U','L','L','S','N','O','T','N','U','L','L','I','K','E','L','S',
  'E','X','C','L','U','D','E','L','E','G','A','T','E','S','C','O','N','S',
  'T','R','A','I','N','T','E','R','S','E','C','T','I','O','N','A','T','U',
  'R','A','L','T','E','R','A','I','S','E','X','C','E','P','T','R','A','N',
  'S','A','C','T','I','O','N','O','T','H','I','N','G','E','N','E','R','A',
  'T','E','D','E','L','E','T','E','M','P','O','R','A','R','Y','M','A','C',
  'R','O','S','S','U','N','I','Q','U','E','R','Y','W','I','N','D','O','W',
  'N','W','I','T','H','O','U','T','E','R','A','N','G','E','X','C','L','U',
  'S','I','V','E','X','I','S','T','S','A','C','C','U','M','U','L','A','T',
  'E','A','T','T','A','C','H','A','V','I','N','G','L','O','B','E','G','I',
  'N','S','T','E','A','D','D','E','T','A','C','H','B','E','T','W','E','E',
  'N','B','I','T','O','R','D','E','R','E','F','E','R','E','N','C','E','S',
  'C','A','S','C','A','D','E','F','A','U','L','T','R','E','E','C','A','S',
  'E','C','O','L','L','A','T','E','C','R','E','A','T','E','C','U','R','R',
  'E','N','T','_','D','A','T','E','I','M','M','E','D','I','A','T','E','J',
  'O','I','N','N','E','R','E','L','E','A','S','E','M','A','T','C','H','M',
  'O','D','U','L','E','P','L','A','N','A','L','Y','Z','E','P','R','A','G',
  'M','A','T','E','R','I','A','L','I','Z','E','D','E','F','E','R','R','E',
  'D','I','S','T','I','N','C','T','R','I','G','G','E','R','E','C','U','R',
  'S','I','V','E','U','P','D','A','T','E','V','I','R','T','U','A','L','W',
  'A','Y','S','W','H','E','N','W','H','E','R','E','N','A','M','E','W','I',
  'T','H','I','N','S','E','R','T','A','B','O','R','T','A','F','T','E','R',
  'E','P','L','A','C','E','A','N','D','R','O','P','A','R','T','I','T','I',
  'O','N','A','U','T','O','I','N','C','R','E','M','E','N','T','C','A','S',
  'T','C','O','L','U','M','N','C','O','M','M','I','T','C','O','N','F','L',
  'I','C','T','C','U','R','R','E','N','T','_','T','I','M','E','S','T','A',
  'M','P','R','E','C','E','D','I','N','G','R','O','U','P','S','F','A','I',
  'L','A','S','T','F','I','L','T','E','R','E','S','T','R','I','C','T','F',
  'I','R','S','T','F','O','L','L','O','W','I','N','G','F','R','O','M','F',
  'U','L','L','I','M','I','T','F','U','N','C','T','I','O','N','I','F','I',
  'N','T','O','T','H','E','R','S','O','V','E','R','E','T','U','R','N','I',
  'N','G','R','E','T','U','R','N','S','R','I','G','H','T','R','O','L','L',
  'B','A','C','K','R','O','W','S','U','N','B','O','U','N','D','E','D','U',
  'N','I','O','N','U','S','I','N','G','V','A','C','U','U','M','V','I','E',
  'W','B','Y','I','N','I','T','I','A','L','L','Y','P','R','I','M','A','R',
  'Y',
};
/* aKWHash[i] is the hash value for the i-th keyword */
static const unsigned char aKWHash[127] = {
   115, 104, 152,  92, 121,  57,   6, 107,  21,   0,  96,  53,   0,
     9,  18,  98,  77, 114,  51, 111,  10, 101, 153,  17,   0,   0,
   158,   0,  39, 138,  97,  26, 123,   0,  72,   0,   0, 140,  90,
     0,  88,  30,   0,  62, 119, 163,   0, 154, 130,   0,   0,  70,
     0, 102,  29,   0,  15,   0,  55,  73,  14,  41,   5,  75,  63,
   126, 139,   0, 133, 103,  80, 161,  76, 137, 134,   0,  71,   0,
    24,  50,   0,  60,   0,   0,   0, 125,  23, 127, 131, 142,  27,
    43, 141,   0, 116,  64,  16, 118, 160,  69, 147, 157, 100,  93,
    36,  58, 143,   0,   0, 124, 146, 148,  82,   0,  45,   0,   0,
   149,   0, 112,  37,  38,   0,  11,  47, 132, 108,
};
/* aKWNext[] forms the hash collision chain.  If aKWHash[i]==0
** then the i-th keyword has no more hash collisions.  Otherwise,
** the next keyword with the same hash is aKWHash[i]-1. */
static const unsigned char aKWNext[164] = {0,
     0,   0,   0,   0,   4,   0,   0,   0,  28,  74,   0,  20,   0,
     0,   0,  65, 150,  25,   0,   0,  66,   0, 122, 129,   0,  13,
     0,   0,   0,   0,  52,   0,   8,   0,   0,   0,  94,   0, 156,
     0,   0,   0, 144,   0, 155,   0,  87,   0,  44,   0, 151,   0,
     0,   0, 159,   0,   0, 136,   0,   0,   0,   0,  40,   0,   0,
     0,  95,   0,  49,  56,  86,  22,   0,   0,   0,   0,   2,   0,
     0,   0,   0,  68,   0,   0,  79,   0,  35,   1,  84,   0,   0,
     0,  33,   0,   0,   0,   0,   0,   0,   0, 145,   0, 120,   0,
     0,   0,   0,  61,  83,  81,   0,   0,   0,   0,   0,   0,  48,
    67,  78,  32,   0,   0,   0,   0,   0,  42,   0,   0,   0,  91,
   117,  59, 162,   3, 128,  12,  31,   0,   0,  89, 110, 135,   0,
     0,   0,   0,   0, 106,  46,   0,   0, 105,   7,  85,   0, 109,
    34,  19,  54, 113,   0,   0,  99,
};
/* synq_perfetto_aKWLen[i] is the length (in bytes) of the i-th keyword */
const unsigned char synq_perfetto_aKWLen[164] = {0,
     7,   7,   5,   4,   6,   8,   3,   6,   2,   3,   5,   4,   4,
     3,   8,   2,   6,   4,   9,   8,   6,   6,   7,   3,   6,   6,
     7,   7,  10,   4,   5,   3,   6,   5,   7,   3,   2,   4,   4,
     4,   7,   9,  10,  12,   9,   2,   7,   5,   5,   6,  11,   6,
     7,   9,   6,   9,   4,   2,   5,   5,   6,   5,   6,   4,   2,
     7,   4,   5,   5,   9,   6,  10,   6,   6,   4,   5,   7,   3,
     6,   7,   5,   5,  10,   7,   3,   7,   4,   4,   7,   6,  12,
     9,   4,   5,   7,   5,   6,   4,   7,   6,  12,   8,   8,   2,
     7,   9,   2,   6,   7,   6,   4,   5,   6,   6,   6,   5,   5,
     7,   3,   4,   9,  13,   2,   2,   4,   6,   6,   8,  17,  12,
     7,   9,   6,   5,   4,   4,   6,   8,   5,   9,   4,   4,   5,
     8,   2,   4,   6,   4,   9,   7,   5,   8,   4,   3,   9,   5,
     5,   6,   4,   2,   9,   3,   7,
};
/* synq_perfetto_aKWOffset[i] is the index into synq_perfetto_zKWText[] of the start of
** the text for the i-th keyword. */
const unsigned short int synq_perfetto_aKWOffset[164] = {0,
     0,   2,   2,   8,   9,  13,  13,  20,  20,  23,  25,  28,  31,
    33,  35,  40,  41,  46,  49,  55,  60,  66,  68,  68,  72,  76,
    79,  84,  89,  98, 100, 104, 107, 109, 114, 114, 114, 117, 120,
   123, 126, 131, 140, 147, 147, 157, 158, 163, 167, 171, 176, 181,
   186, 192, 200, 204, 204, 208, 213, 215, 220, 223, 228, 231, 231,
   235, 235, 239, 243, 247, 255, 261, 271, 276, 281, 284, 287, 292,
   294, 300, 307, 310, 314, 324, 325, 329, 335, 339, 343, 350, 356,
   368, 377, 379, 383, 390, 395, 401, 403, 410, 414, 425, 432, 433,
   439, 445, 454, 454, 460, 465, 471, 475, 478, 484, 488, 494, 499,
   503, 510, 512, 515, 524, 526, 528, 537, 541, 547, 553, 561, 561,
   561, 577, 585, 585, 591, 594, 598, 603, 611, 616, 625, 629, 632,
   637, 645, 647, 650, 656, 659, 668, 675, 680, 688, 688, 692, 701,
   706, 711, 717, 721, 723, 728, 732,
};
/* synq_perfetto_aKWCode[i] is the parser symbol code for the i-th keyword */
const unsigned char synq_perfetto_aKWCode[164] = {0,
  SYNTAQLITE_TK_REINDEX,    SYNTAQLITE_TK_INDEXED,    SYNTAQLITE_TK_INDEX,      SYNTAQLITE_TK_DESC,       SYNTAQLITE_TK_ESCAPE,     
  SYNTAQLITE_TK_PERFETTO,   SYNTAQLITE_TK_PER,        SYNTAQLITE_TK_OFFSET,     SYNTAQLITE_TK_OF,         SYNTAQLITE_TK_SET,        
  SYNTAQLITE_TK_TABLE,      SYNTAQLITE_TK_JOIN_KW,    SYNTAQLITE_TK_THEN,       SYNTAQLITE_TK_END,        SYNTAQLITE_TK_DATABASE,   
  SYNTAQLITE_TK_AS,         SYNTAQLITE_TK_SELECT,     SYNTAQLITE_TK_TIES,       SYNTAQLITE_TK_SAVEPOINT,  SYNTAQLITE_TK_INTERVAL,   
  SYNTAQLITE_TK_VALUES,     SYNTAQLITE_TK_BEFORE,     SYNTAQLITE_TK_FOREIGN,    SYNTAQLITE_TK_FOR,        SYNTAQLITE_TK_IGNORE,     
  SYNTAQLITE_TK_LIKE_KW,    SYNTAQLITE_TK_EXPLAIN,    SYNTAQLITE_TK_INCLUDE,    SYNTAQLITE_TK_DEFERRABLE, SYNTAQLITE_TK_EACH,       
  SYNTAQLITE_TK_CHECK,      SYNTAQLITE_TK_KEY,        SYNTAQLITE_TK_ISNULL,     SYNTAQLITE_TK_NULLS,      SYNTAQLITE_TK_NOTNULL,    
  SYNTAQLITE_TK_NOT,        SYNTAQLITE_TK_NO,         SYNTAQLITE_TK_NULL,       SYNTAQLITE_TK_LIKE_KW,    SYNTAQLITE_TK_ELSE,       
  SYNTAQLITE_TK_EXCLUDE,    SYNTAQLITE_TK_DELEGATES,  SYNTAQLITE_TK_CONSTRAINT, SYNTAQLITE_TK_INTERSECTION, SYNTAQLITE_TK_INTERSECT,  
  SYNTAQLITE_TK_ON,         SYNTAQLITE_TK_JOIN_KW,    SYNTAQLITE_TK_ALTER,      SYNTAQLITE_TK_RAISE,      SYNTAQLITE_TK_EXCEPT,     
  SYNTAQLITE_TK_TRANSACTION,SYNTAQLITE_TK_ACTION,     SYNTAQLITE_TK_NOTHING,    SYNTAQLITE_TK_GENERATED,  SYNTAQLITE_TK_DELETE,     
  SYNTAQLITE_TK_TEMP,       SYNTAQLITE_TK_TEMP,       SYNTAQLITE_TK_OR,         SYNTAQLITE_TK_MACRO,      SYNTAQLITE_TK_JOIN_KW,    
  SYNTAQLITE_TK_UNIQUE,     SYNTAQLITE_TK_QUERY,      SYNTAQLITE_TK_WINDOW,     SYNTAQLITE_TK_DOWN,       SYNTAQLITE_TK_DO,         
  SYNTAQLITE_TK_WITHOUT,    SYNTAQLITE_TK_WITH,       SYNTAQLITE_TK_JOIN_KW,    SYNTAQLITE_TK_RANGE,      SYNTAQLITE_TK_EXCLUSIVE,  
  SYNTAQLITE_TK_EXISTS,     SYNTAQLITE_TK_ACCUMULATE, SYNTAQLITE_TK_ATTACH,     SYNTAQLITE_TK_HAVING,     SYNTAQLITE_TK_LIKE_KW,    
  SYNTAQLITE_TK_BEGIN,      SYNTAQLITE_TK_INSTEAD,    SYNTAQLITE_TK_ADD,        SYNTAQLITE_TK_DETACH,     SYNTAQLITE_TK_BETWEEN,    
  SYNTAQLITE_TK_BITOR,      SYNTAQLITE_TK_ORDER,      SYNTAQLITE_TK_REFERENCES, SYNTAQLITE_TK_CASCADE,    SYNTAQLITE_TK_ASC,        
  SYNTAQLITE_TK_DEFAULT,    SYNTAQLITE_TK_TREE,       SYNTAQLITE_TK_CASE,       SYNTAQLITE_TK_COLLATE,    SYNTAQLITE_TK_CREATE,     
  SYNTAQLITE_TK_CTIME_KW,   SYNTAQLITE_TK_IMMEDIATE,  SYNTAQLITE_TK_JOIN,       SYNTAQLITE_TK_JOIN_KW,    SYNTAQLITE_TK_RELEASE,    
  SYNTAQLITE_TK_MATCH,      SYNTAQLITE_TK_MODULE,     SYNTAQLITE_TK_PLAN,       SYNTAQLITE_TK_ANALYZE,    SYNTAQLITE_TK_PRAGMA,     
  SYNTAQLITE_TK_MATERIALIZED, SYNTAQLITE_TK_DEFERRED,   SYNTAQLITE_TK_DISTINCT,   SYNTAQLITE_TK_IS,         SYNTAQLITE_TK_TRIGGER,    
  SYNTAQLITE_TK_RECURSIVE,  SYNTAQLITE_TK_UP,         SYNTAQLITE_TK_UPDATE,     SYNTAQLITE_TK_VIRTUAL,    SYNTAQLITE_TK_ALWAYS,     
  SYNTAQLITE_TK_WHEN,       SYNTAQLITE_TK_WHERE,      SYNTAQLITE_TK_RENAME,     SYNTAQLITE_TK_WITHIN,     SYNTAQLITE_TK_INSERT,     
  SYNTAQLITE_TK_ABORT,      SYNTAQLITE_TK_AFTER,      SYNTAQLITE_TK_REPLACE,    SYNTAQLITE_TK_AND,        SYNTAQLITE_TK_DROP,       
  SYNTAQLITE_TK_PARTITION,  SYNTAQLITE_TK_AUTOINCR,   SYNTAQLITE_TK_TO,         SYNTAQLITE_TK_IN,         SYNTAQLITE_TK_CAST,       
  SYNTAQLITE_TK_COLUMNKW,   SYNTAQLITE_TK_COMMIT,     SYNTAQLITE_TK_CONFLICT,   SYNTAQLITE_TK_CTIME_KW,   SYNTAQLITE_TK_CTIME_KW,   
  SYNTAQLITE_TK_CURRENT,    SYNTAQLITE_TK_PRECEDING,  SYNTAQLITE_TK_GROUPS,     SYNTAQLITE_TK_GROUP,      SYNTAQLITE_TK_FAIL,       
  SYNTAQLITE_TK_LAST,       SYNTAQLITE_TK_FILTER,     SYNTAQLITE_TK_RESTRICT,   SYNTAQLITE_TK_FIRST,      SYNTAQLITE_TK_FOLLOWING,  
  SYNTAQLITE_TK_FROM,       SYNTAQLITE_TK_JOIN_KW,    SYNTAQLITE_TK_LIMIT,      SYNTAQLITE_TK_FUNCTION,   SYNTAQLITE_TK_IF,         
  SYNTAQLITE_TK_INTO,       SYNTAQLITE_TK_OTHERS,     SYNTAQLITE_TK_OVER,       SYNTAQLITE_TK_RETURNING,  SYNTAQLITE_TK_RETURNS,    
  SYNTAQLITE_TK_JOIN_KW,    SYNTAQLITE_TK_ROLLBACK,   SYNTAQLITE_TK_ROWS,       SYNTAQLITE_TK_ROW,        SYNTAQLITE_TK_UNBOUNDED,  
  SYNTAQLITE_TK_UNION,      SYNTAQLITE_TK_USING,      SYNTAQLITE_TK_VACUUM,     SYNTAQLITE_TK_VIEW,       SYNTAQLITE_TK_BY,         
  SYNTAQLITE_TK_INITIALLY,  SYNTAQLITE_TK_ALL,        SYNTAQLITE_TK_PRIMARY,    
};
/* Hash table decoded:
**   0: INSERT
**   1: IS
**   2: ROLLBACK TRIGGER
**   3: IMMEDIATE
**   4: PARTITION
**   5: TEMP
**   6: PERFETTO
**   7: UP
**   8: VALUES WITHOUT
**   9:
**  10: MATCH
**  11: NOTHING
**  12:
**  13: OF INCLUDE
**  14: TIES IGNORE
**  15: PLAN
**  16: INSTEAD INDEXED
**  17: WITHIN
**  18: TRANSACTION RIGHT
**  19: WHEN
**  20: SET HAVING
**  21: MATERIALIZED IF
**  22: ROWS PER
**  23: SELECT RETURNS
**  24:
**  25:
**  26: VACUUM SAVEPOINT
**  27:
**  28: LIKE UNION VIRTUAL REFERENCES
**  29: RESTRICT
**  30: MODULE
**  31: REGEXP THEN
**  32: TO
**  33:
**  34: ACCUMULATE BEFORE
**  35:
**  36:
**  37: FOLLOWING COLLATE CASCADE
**  38: CREATE
**  39:
**  40: CASE REINDEX
**  41: EACH
**  42:
**  43: QUERY
**  44: AND ADD
**  45: PRIMARY ANALYZE
**  46:
**  47: ROW ASC DETACH
**  48: CURRENT_TIME CURRENT_DATE
**  49:
**  50:
**  51: EXCLUSIVE TEMPORARY
**  52:
**  53: DEFERRED
**  54: DEFERRABLE
**  55:
**  56: DATABASE
**  57:
**  58: DELETE VIEW GENERATED
**  59: ATTACH
**  60: END
**  61: EXCLUDE
**  62: ESCAPE DESC
**  63: GLOB
**  64: WINDOW ELSE
**  65: COLUMN DELEGATES
**  66: FIRST
**  67:
**  68: GROUPS ALL
**  69: DISTINCT DROP KEY
**  70: BETWEEN
**  71: INITIALLY
**  72: BEGIN
**  73: FILTER CHECK ACTION
**  74: GROUP INDEX
**  75:
**  76: EXISTS DEFAULT
**  77:
**  78: FOR CURRENT_TIMESTAMP
**  79: EXCEPT
**  80:
**  81: CROSS
**  82:
**  83:
**  84:
**  85: CAST
**  86: FOREIGN AUTOINCREMENT
**  87: COMMIT
**  88: CURRENT AFTER ALTER
**  89: FULL FAIL CONFLICT
**  90: EXPLAIN
**  91: CONSTRAINT FUNCTION
**  92: FROM ALWAYS BITOR
**  93:
**  94: ABORT
**  95: DOWN
**  96: AS DO
**  97: REPLACE WITH RELEASE
**  98: BY RENAME
**  99: RANGE RAISE INTERSECTION
** 100: OTHERS
** 101: USING NULLS
** 102: PRAGMA
** 103: JOIN ISNULL OFFSET
** 104: NOT
** 105: OR LAST LEFT INTERVAL
** 106: LIMIT
** 107:
** 108:
** 109: IN
** 110: INTO
** 111: OVER RECURSIVE
** 112: ORDER OUTER
** 113:
** 114: INTERSECT UNBOUNDED
** 115:
** 116:
** 117: RETURNING ON
** 118:
** 119: WHERE
** 120: NO INNER
** 121: NULL
** 122:
** 123: TABLE
** 124: NATURAL TREE NOTNULL
** 125: PRECEDING MACRO
** 126: UPDATE UNIQUE
*/
/* Check to see if z[0..n-1] is a keyword. If it is, write the
** parser symbol code for that keyword into *pType.  Always
** return the integer n (the length of the token). */
static const int32_t synq_perfetto_aKWSince[164] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,3028000,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,3030000,0,0,0,0,
  0,0,3028000,0,0,0,0,0,0,0,0,0,0,
  0,3024000,3031000,0,0,0,0,0,0,0,0,3025000,0,
  3024000,0,0,0,3025000,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,3035000,0,0,
  0,0,0,0,0,0,3031000,0,0,0,3047000,0,0,
  0,0,0,0,3025000,0,0,0,0,0,0,0,0,
  0,3025000,3025000,3028000,0,0,3030000,3025000,0,3030000,3025000,0,0,
  0,0,0,0,3028000,3025000,3035000,0,0,0,3025000,0,3025000,
  0,0,0,0,0,0,0,0
};
static const int8_t synq_perfetto_aKWCFlag[164] = {
  -1,11,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,2,-1,-1,19,-1,-1,-1,15,8,15,-1,
  -1,7,-1,8,15,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,19,-1,-1,-1,5,-1,-1,-1,15,5,-1,
  8,-1,-1,-1,-1,-1,-1,-1,-1,-1,7,19,-1,
  -1,-1,6,-1,19,-1,-1,-1,2,-1,-1,-1,15,
  -1,2,-1,-1,-1,8,8,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,7,1,10,6,-1,-1,
  -1,15,6,-1,-1,18,9,-1,-1,-1,20,-1,-1,
  15,-1,-1,-1,19,3,-1,-1,4,-1,-1,-1,-1,
  -1,19,19,19,-1,-1,-1,19,8,-1,19,-1,-1,
  -1,-1,-1,-1,19,19,12,-1,-1,-1,-1,15,19,
  5,-1,-1,17,-1,8,-1,-1
};
static const uint8_t synq_perfetto_aKWCFlagPolarity[164] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,1,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0
};

int synq_sqlite3_keywordCode(const SyntaqliteDialect *env, const char *z, int n, int *pType){
  int i, j;
  const char *zKW;
  assert( n>=2 );
  i = ((charMap(z[0])*4) ^ (charMap(z[n-1])*3) ^ n*1) % 127;
  for(i=(int)aKWHash[i]; i>0; i=aKWNext[i]){
    if( synq_perfetto_aKWLen[i]!=n ) continue;
    zKW = &synq_perfetto_zKWText[synq_perfetto_aKWOffset[i]];
#ifdef SQLITE_ASCII
    if( (z[0]&~0x20)!=zKW[0] ) continue;
    if( (z[1]&~0x20)!=zKW[1] ) continue;
    j = 2;
    while( j<n && (z[j]&~0x20)==zKW[j] ){ j++; }
#endif
#ifdef SQLITE_EBCDIC
    if( toupper(z[0])!=zKW[0] ) continue;
    if( toupper(z[1])!=zKW[1] ) continue;
    j = 2;
    while( j<n && toupper(z[j])==zKW[j] ){ j++; }
#endif
    if( j<n ) continue;
    testcase( i==1 ); /* REINDEX */
    testcase( i==2 ); /* INDEXED */
    testcase( i==3 ); /* INDEX */
    testcase( i==4 ); /* DESC */
    testcase( i==5 ); /* ESCAPE */
    testcase( i==6 ); /* PERFETTO */
    testcase( i==7 ); /* PER */
    testcase( i==8 ); /* OFFSET */
    testcase( i==9 ); /* OF */
    testcase( i==10 ); /* SET */
    testcase( i==11 ); /* TABLE */
    testcase( i==12 ); /* LEFT */
    testcase( i==13 ); /* THEN */
    testcase( i==14 ); /* END */
    testcase( i==15 ); /* DATABASE */
    testcase( i==16 ); /* AS */
    testcase( i==17 ); /* SELECT */
    testcase( i==18 ); /* TIES */
    testcase( i==19 ); /* SAVEPOINT */
    testcase( i==20 ); /* INTERVAL */
    testcase( i==21 ); /* VALUES */
    testcase( i==22 ); /* BEFORE */
    testcase( i==23 ); /* FOREIGN */
    testcase( i==24 ); /* FOR */
    testcase( i==25 ); /* IGNORE */
    testcase( i==26 ); /* REGEXP */
    testcase( i==27 ); /* EXPLAIN */
    testcase( i==28 ); /* INCLUDE */
    testcase( i==29 ); /* DEFERRABLE */
    testcase( i==30 ); /* EACH */
    testcase( i==31 ); /* CHECK */
    testcase( i==32 ); /* KEY */
    testcase( i==33 ); /* ISNULL */
    testcase( i==34 ); /* NULLS */
    testcase( i==35 ); /* NOTNULL */
    testcase( i==36 ); /* NOT */
    testcase( i==37 ); /* NO */
    testcase( i==38 ); /* NULL */
    testcase( i==39 ); /* LIKE */
    testcase( i==40 ); /* ELSE */
    testcase( i==41 ); /* EXCLUDE */
    testcase( i==42 ); /* DELEGATES */
    testcase( i==43 ); /* CONSTRAINT */
    testcase( i==44 ); /* INTERSECTION */
    testcase( i==45 ); /* INTERSECT */
    testcase( i==46 ); /* ON */
    testcase( i==47 ); /* NATURAL */
    testcase( i==48 ); /* ALTER */
    testcase( i==49 ); /* RAISE */
    testcase( i==50 ); /* EXCEPT */
    testcase( i==51 ); /* TRANSACTION */
    testcase( i==52 ); /* ACTION */
    testcase( i==53 ); /* NOTHING */
    testcase( i==54 ); /* GENERATED */
    testcase( i==55 ); /* DELETE */
    testcase( i==56 ); /* TEMPORARY */
    testcase( i==57 ); /* TEMP */
    testcase( i==58 ); /* OR */
    testcase( i==59 ); /* MACRO */
    testcase( i==60 ); /* CROSS */
    testcase( i==61 ); /* UNIQUE */
    testcase( i==62 ); /* QUERY */
    testcase( i==63 ); /* WINDOW */
    testcase( i==64 ); /* DOWN */
    testcase( i==65 ); /* DO */
    testcase( i==66 ); /* WITHOUT */
    testcase( i==67 ); /* WITH */
    testcase( i==68 ); /* OUTER */
    testcase( i==69 ); /* RANGE */
    testcase( i==70 ); /* EXCLUSIVE */
    testcase( i==71 ); /* EXISTS */
    testcase( i==72 ); /* ACCUMULATE */
    testcase( i==73 ); /* ATTACH */
    testcase( i==74 ); /* HAVING */
    testcase( i==75 ); /* GLOB */
    testcase( i==76 ); /* BEGIN */
    testcase( i==77 ); /* INSTEAD */
    testcase( i==78 ); /* ADD */
    testcase( i==79 ); /* DETACH */
    testcase( i==80 ); /* BETWEEN */
    testcase( i==81 ); /* BITOR */
    testcase( i==82 ); /* ORDER */
    testcase( i==83 ); /* REFERENCES */
    testcase( i==84 ); /* CASCADE */
    testcase( i==85 ); /* ASC */
    testcase( i==86 ); /* DEFAULT */
    testcase( i==87 ); /* TREE */
    testcase( i==88 ); /* CASE */
    testcase( i==89 ); /* COLLATE */
    testcase( i==90 ); /* CREATE */
    testcase( i==91 ); /* CURRENT_DATE */
    testcase( i==92 ); /* IMMEDIATE */
    testcase( i==93 ); /* JOIN */
    testcase( i==94 ); /* INNER */
    testcase( i==95 ); /* RELEASE */
    testcase( i==96 ); /* MATCH */
    testcase( i==97 ); /* MODULE */
    testcase( i==98 ); /* PLAN */
    testcase( i==99 ); /* ANALYZE */
    testcase( i==100 ); /* PRAGMA */
    testcase( i==101 ); /* MATERIALIZED */
    testcase( i==102 ); /* DEFERRED */
    testcase( i==103 ); /* DISTINCT */
    testcase( i==104 ); /* IS */
    testcase( i==105 ); /* TRIGGER */
    testcase( i==106 ); /* RECURSIVE */
    testcase( i==107 ); /* UP */
    testcase( i==108 ); /* UPDATE */
    testcase( i==109 ); /* VIRTUAL */
    testcase( i==110 ); /* ALWAYS */
    testcase( i==111 ); /* WHEN */
    testcase( i==112 ); /* WHERE */
    testcase( i==113 ); /* RENAME */
    testcase( i==114 ); /* WITHIN */
    testcase( i==115 ); /* INSERT */
    testcase( i==116 ); /* ABORT */
    testcase( i==117 ); /* AFTER */
    testcase( i==118 ); /* REPLACE */
    testcase( i==119 ); /* AND */
    testcase( i==120 ); /* DROP */
    testcase( i==121 ); /* PARTITION */
    testcase( i==122 ); /* AUTOINCREMENT */
    testcase( i==123 ); /* TO */
    testcase( i==124 ); /* IN */
    testcase( i==125 ); /* CAST */
    testcase( i==126 ); /* COLUMN */
    testcase( i==127 ); /* COMMIT */
    testcase( i==128 ); /* CONFLICT */
    testcase( i==129 ); /* CURRENT_TIMESTAMP */
    testcase( i==130 ); /* CURRENT_TIME */
    testcase( i==131 ); /* CURRENT */
    testcase( i==132 ); /* PRECEDING */
    testcase( i==133 ); /* GROUPS */
    testcase( i==134 ); /* GROUP */
    testcase( i==135 ); /* FAIL */
    testcase( i==136 ); /* LAST */
    testcase( i==137 ); /* FILTER */
    testcase( i==138 ); /* RESTRICT */
    testcase( i==139 ); /* FIRST */
    testcase( i==140 ); /* FOLLOWING */
    testcase( i==141 ); /* FROM */
    testcase( i==142 ); /* FULL */
    testcase( i==143 ); /* LIMIT */
    testcase( i==144 ); /* FUNCTION */
    testcase( i==145 ); /* IF */
    testcase( i==146 ); /* INTO */
    testcase( i==147 ); /* OTHERS */
    testcase( i==148 ); /* OVER */
    testcase( i==149 ); /* RETURNING */
    testcase( i==150 ); /* RETURNS */
    testcase( i==151 ); /* RIGHT */
    testcase( i==152 ); /* ROLLBACK */
    testcase( i==153 ); /* ROWS */
    testcase( i==154 ); /* ROW */
    testcase( i==155 ); /* UNBOUNDED */
    testcase( i==156 ); /* UNION */
    testcase( i==157 ); /* USING */
    testcase( i==158 ); /* VACUUM */
    testcase( i==159 ); /* VIEW */
    testcase( i==160 ); /* BY */
    testcase( i==161 ); /* INITIALLY */
    testcase( i==162 ); /* ALL */
    testcase( i==163 ); /* PRIMARY */
    /* Version check: skip keywords newer than target version. */
    if( synq_perfetto_aKWSince[i] != 0 && SYNQ_VER_LT(env, synq_perfetto_aKWSince[i]) ){
      break;
    }
    /* CFlag check with polarity. */
    if( synq_perfetto_aKWCFlag[i] >= 0 ){
      int flag_set = SYNQ_HAS_CFLAG(env, synq_perfetto_aKWCFlag[i]);
      int is_enable = synq_perfetto_aKWCFlagPolarity[i];
      if( flag_set != is_enable ){
        break;
      }
    }
    *pType = synq_perfetto_aKWCode[i];
    break;
  }
  return n;
}






const unsigned int synq_perfetto_nKeyword = sizeof(synq_perfetto_aKWCode) / sizeof(synq_perfetto_aKWCode[0]);

/* ======== end: csrc/sqlite_keyword.c ======== */

/* ======== begin: csrc/dialect_builder.h ======== */
#ifndef SYNTAQLITE_PERFETTO_DIALECT_BUILDER_H
#define SYNTAQLITE_PERFETTO_DIALECT_BUILDER_H
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.
//
// @generated by syntaqlite-buildtools — DO NOT EDIT



#ifdef __cplusplus
extern "C" {
#endif

// ============ Builder Functions ============

static inline uint32_t synq_parse_aggregate_function_call(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan func_name,
    SyntaqliteAggregateFunctionCallFlags flags,
    uint32_t args,
    uint32_t orderby,
    uint32_t filter_clause,
    uint32_t over_clause
) {
    return synq_parse_build(ctx,
        &(SyntaqliteAggregateFunctionCall){
            .tag = SYNTAQLITE_NODE_AGGREGATE_FUNCTION_CALL,
            .func_name = func_name,
            .flags = flags,
            .args = args,
            .orderby = orderby,
            .filter_clause = filter_clause,
            .over_clause = over_clause
        }, (uint32_t)sizeof(SyntaqliteAggregateFunctionCall));
}

static inline uint32_t synq_parse_ordered_set_function_call(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan func_name,
    SyntaqliteAggregateFunctionCallFlags flags,
    uint32_t args,
    uint32_t orderby_expr,
    uint32_t filter_clause,
    uint32_t over_clause
) {
    return synq_parse_build(ctx,
        &(SyntaqliteOrderedSetFunctionCall){
            .tag = SYNTAQLITE_NODE_ORDERED_SET_FUNCTION_CALL,
            .func_name = func_name,
            .flags = flags,
            .args = args,
            .orderby_expr = orderby_expr,
            .filter_clause = filter_clause,
            .over_clause = over_clause
        }, (uint32_t)sizeof(SyntaqliteOrderedSetFunctionCall));
}

static inline uint32_t synq_parse_cast_expr(
    SynqParseCtx *ctx,
    uint32_t expr,
    SyntaqliteTextSpan type_name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCastExpr){
            .tag = SYNTAQLITE_NODE_CAST_EXPR,
            .expr = expr,
            .type_name = type_name
        }, (uint32_t)sizeof(SyntaqliteCastExpr));
}

static inline uint32_t synq_parse_column_ref(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan column,
    SyntaqliteTextSpan table,
    SyntaqliteTextSpan schema
) {
    return synq_parse_build(ctx,
        &(SyntaqliteColumnRef){
            .tag = SYNTAQLITE_NODE_COLUMN_REF,
            .column = column,
            .table = table,
            .schema = schema
        }, (uint32_t)sizeof(SyntaqliteColumnRef));
}

static inline uint32_t synq_parse_compound_select(
    SynqParseCtx *ctx,
    SyntaqliteCompoundOp op,
    uint32_t left,
    uint32_t right,
    uint32_t orderby,
    uint32_t limit_clause
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCompoundSelect){
            .tag = SYNTAQLITE_NODE_COMPOUND_SELECT,
            .op = op,
            .left = left,
            .right = right,
            .orderby = orderby,
            .limit_clause = limit_clause
        }, (uint32_t)sizeof(SyntaqliteCompoundSelect));
}

static inline uint32_t synq_parse_subquery_expr(
    SynqParseCtx *ctx,
    uint32_t select
) {
    return synq_parse_build(ctx,
        &(SyntaqliteSubqueryExpr){
            .tag = SYNTAQLITE_NODE_SUBQUERY_EXPR,
            .select = select
        }, (uint32_t)sizeof(SyntaqliteSubqueryExpr));
}

static inline uint32_t synq_parse_exists_expr(
    SynqParseCtx *ctx,
    uint32_t select
) {
    return synq_parse_build(ctx,
        &(SyntaqliteExistsExpr){
            .tag = SYNTAQLITE_NODE_EXISTS_EXPR,
            .select = select
        }, (uint32_t)sizeof(SyntaqliteExistsExpr));
}

static inline uint32_t synq_parse_in_expr(
    SynqParseCtx *ctx,
    SyntaqliteBool negated,
    SyntaqliteBool bare_source,
    uint32_t operand,
    uint32_t source
) {
    return synq_parse_build(ctx,
        &(SyntaqliteInExpr){
            .tag = SYNTAQLITE_NODE_IN_EXPR,
            .negated = negated,
            .bare_source = bare_source,
            .operand = operand,
            .source = source
        }, (uint32_t)sizeof(SyntaqliteInExpr));
}

static inline uint32_t synq_parse_is_expr(
    SynqParseCtx *ctx,
    SyntaqliteIsOp op,
    uint32_t left,
    uint32_t right
) {
    return synq_parse_build(ctx,
        &(SyntaqliteIsExpr){
            .tag = SYNTAQLITE_NODE_IS_EXPR,
            .op = op,
            .left = left,
            .right = right
        }, (uint32_t)sizeof(SyntaqliteIsExpr));
}

static inline uint32_t synq_parse_between_expr(
    SynqParseCtx *ctx,
    SyntaqliteBool negated,
    uint32_t operand,
    uint32_t low,
    uint32_t high
) {
    return synq_parse_build(ctx,
        &(SyntaqliteBetweenExpr){
            .tag = SYNTAQLITE_NODE_BETWEEN_EXPR,
            .negated = negated,
            .operand = operand,
            .low = low,
            .high = high
        }, (uint32_t)sizeof(SyntaqliteBetweenExpr));
}

static inline uint32_t synq_parse_like_expr(
    SynqParseCtx *ctx,
    SyntaqliteBool negated,
    SyntaqliteLikeKeyword keyword,
    uint32_t operand,
    uint32_t pattern,
    uint32_t escape
) {
    return synq_parse_build(ctx,
        &(SyntaqliteLikeExpr){
            .tag = SYNTAQLITE_NODE_LIKE_EXPR,
            .negated = negated,
            .keyword = keyword,
            .operand = operand,
            .pattern = pattern,
            .escape = escape
        }, (uint32_t)sizeof(SyntaqliteLikeExpr));
}

static inline uint32_t synq_parse_case_expr(
    SynqParseCtx *ctx,
    uint32_t operand,
    uint32_t else_expr,
    uint32_t whens
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCaseExpr){
            .tag = SYNTAQLITE_NODE_CASE_EXPR,
            .operand = operand,
            .else_expr = else_expr,
            .whens = whens
        }, (uint32_t)sizeof(SyntaqliteCaseExpr));
}

static inline uint32_t synq_parse_case_when(
    SynqParseCtx *ctx,
    uint32_t when_expr,
    uint32_t then_expr
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCaseWhen){
            .tag = SYNTAQLITE_NODE_CASE_WHEN,
            .when_expr = when_expr,
            .then_expr = then_expr
        }, (uint32_t)sizeof(SyntaqliteCaseWhen));
}

static inline uint32_t synq_parse_foreign_key_option(
    SynqParseCtx *ctx,
    SyntaqliteForeignKeyOptionKind kind,
    SyntaqliteForeignKeyAction action,
    SyntaqliteTextSpan match_name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteForeignKeyOption){
            .tag = SYNTAQLITE_NODE_FOREIGN_KEY_OPTION,
            .kind = kind,
            .action = action,
            .match_name = match_name
        }, (uint32_t)sizeof(SyntaqliteForeignKeyOption));
}

static inline uint32_t synq_parse_foreign_key_clause(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan ref_table,
    uint32_t ref_columns,
    uint32_t options,
    SyntaqliteDeferrable deferrable,
    SyntaqliteInitialDeferMode initial_defer
) {
    return synq_parse_build(ctx,
        &(SyntaqliteForeignKeyClause){
            .tag = SYNTAQLITE_NODE_FOREIGN_KEY_CLAUSE,
            .ref_table = ref_table,
            .ref_columns = ref_columns,
            .options = options,
            .deferrable = deferrable,
            .initial_defer = initial_defer
        }, (uint32_t)sizeof(SyntaqliteForeignKeyClause));
}

static inline uint32_t synq_parse_column_constraint(
    SynqParseCtx *ctx,
    SyntaqliteColumnConstraintType kind,
    SyntaqliteConflictAction onconf,
    SyntaqliteSortOrder sort_order,
    SyntaqliteBool is_autoincrement,
    SyntaqliteTextSpan collation_name,
    SyntaqliteGeneratedColumnStorage generated_storage,
    SyntaqliteDeferrable deferrable,
    SyntaqliteInitialDeferMode initial_defer,
    SyntaqliteBool default_has_parens,
    SyntaqliteBool generated_always,
    uint32_t default_expr,
    uint32_t check_expr,
    uint32_t generated_expr,
    uint32_t fk_clause
) {
    return synq_parse_build(ctx,
        &(SyntaqliteColumnConstraint){
            .tag = SYNTAQLITE_NODE_COLUMN_CONSTRAINT,
            .kind = kind,
            .onconf = onconf,
            .sort_order = sort_order,
            .is_autoincrement = is_autoincrement,
            .collation_name = collation_name,
            .generated_storage = generated_storage,
            .deferrable = deferrable,
            .initial_defer = initial_defer,
            .default_has_parens = default_has_parens,
            .generated_always = generated_always,
            .default_expr = default_expr,
            .check_expr = check_expr,
            .generated_expr = generated_expr,
            .fk_clause = fk_clause
        }, (uint32_t)sizeof(SyntaqliteColumnConstraint));
}

static inline uint32_t synq_parse_constraint_name_declaration(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteConstraintNameDeclaration){
            .tag = SYNTAQLITE_NODE_CONSTRAINT_NAME_DECLARATION,
            .name = name
        }, (uint32_t)sizeof(SyntaqliteConstraintNameDeclaration));
}

static inline uint32_t synq_parse_column_def(
    SynqParseCtx *ctx,
    uint32_t column_name,
    SyntaqliteTextSpan type_name,
    uint32_t constraints
) {
    return synq_parse_build(ctx,
        &(SyntaqliteColumnDef){
            .tag = SYNTAQLITE_NODE_COLUMN_DEF,
            .column_name = column_name,
            .type_name = type_name,
            .constraints = constraints
        }, (uint32_t)sizeof(SyntaqliteColumnDef));
}

static inline uint32_t synq_parse_table_constraint(
    SynqParseCtx *ctx,
    SyntaqliteTableConstraintType kind,
    SyntaqliteConflictAction onconf,
    SyntaqliteBool is_autoincrement,
    uint32_t pk_columns,
    uint32_t fk_columns,
    uint32_t check_expr,
    uint32_t fk_clause
) {
    return synq_parse_build(ctx,
        &(SyntaqliteTableConstraint){
            .tag = SYNTAQLITE_NODE_TABLE_CONSTRAINT,
            .kind = kind,
            .onconf = onconf,
            .is_autoincrement = is_autoincrement,
            .pk_columns = pk_columns,
            .fk_columns = fk_columns,
            .check_expr = check_expr,
            .fk_clause = fk_clause
        }, (uint32_t)sizeof(SyntaqliteTableConstraint));
}

static inline uint32_t synq_parse_create_table_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan table_name,
    SyntaqliteTextSpan schema,
    SyntaqliteTemporaryQualifier temporary,
    SyntaqliteBool if_not_exists,
    SyntaqliteCreateTableStmtFlags flags,
    uint32_t columns,
    uint32_t table_constraints,
    uint32_t as_select
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreateTableStmt){
            .tag = SYNTAQLITE_NODE_CREATE_TABLE_STMT,
            .table_name = table_name,
            .schema = schema,
            .temporary = temporary,
            .if_not_exists = if_not_exists,
            .flags = flags,
            .columns = columns,
            .table_constraints = table_constraints,
            .as_select = as_select
        }, (uint32_t)sizeof(SyntaqliteCreateTableStmt));
}

static inline uint32_t synq_parse_cte_definition(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan cte_name,
    SyntaqliteMaterialized materialized,
    uint32_t columns,
    uint32_t select
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCteDefinition){
            .tag = SYNTAQLITE_NODE_CTE_DEFINITION,
            .cte_name = cte_name,
            .materialized = materialized,
            .columns = columns,
            .select = select
        }, (uint32_t)sizeof(SyntaqliteCteDefinition));
}

static inline uint32_t synq_parse_with_clause(
    SynqParseCtx *ctx,
    SyntaqliteBool recursive,
    uint32_t ctes,
    uint32_t select
) {
    return synq_parse_build(ctx,
        &(SyntaqliteWithClause){
            .tag = SYNTAQLITE_NODE_WITH_CLAUSE,
            .recursive = recursive,
            .ctes = ctes,
            .select = select
        }, (uint32_t)sizeof(SyntaqliteWithClause));
}

static inline uint32_t synq_parse_upsert_clause(
    SynqParseCtx *ctx,
    uint32_t columns,
    uint32_t target_where,
    SyntaqliteUpsertAction action,
    uint32_t setlist,
    uint32_t update_where
) {
    return synq_parse_build(ctx,
        &(SyntaqliteUpsertClause){
            .tag = SYNTAQLITE_NODE_UPSERT_CLAUSE,
            .columns = columns,
            .target_where = target_where,
            .action = action,
            .setlist = setlist,
            .update_where = update_where
        }, (uint32_t)sizeof(SyntaqliteUpsertClause));
}

static inline uint32_t synq_parse_delete_stmt(
    SynqParseCtx *ctx,
    uint32_t with_ctes,
    SyntaqliteBool with_recursive,
    uint32_t table,
    SyntaqliteIndexHint index_hint,
    SyntaqliteTextSpan index_name,
    uint32_t where_clause,
    uint32_t orderby,
    uint32_t limit_clause,
    uint32_t returning
) {
    return synq_parse_build(ctx,
        &(SyntaqliteDeleteStmt){
            .tag = SYNTAQLITE_NODE_DELETE_STMT,
            .with_ctes = with_ctes,
            .with_recursive = with_recursive,
            .table = table,
            .index_hint = index_hint,
            .index_name = index_name,
            .where_clause = where_clause,
            .orderby = orderby,
            .limit_clause = limit_clause,
            .returning = returning
        }, (uint32_t)sizeof(SyntaqliteDeleteStmt));
}

static inline uint32_t synq_parse_set_clause(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan column,
    uint32_t columns,
    uint32_t value
) {
    return synq_parse_build(ctx,
        &(SyntaqliteSetClause){
            .tag = SYNTAQLITE_NODE_SET_CLAUSE,
            .column = column,
            .columns = columns,
            .value = value
        }, (uint32_t)sizeof(SyntaqliteSetClause));
}

static inline uint32_t synq_parse_update_stmt(
    SynqParseCtx *ctx,
    uint32_t with_ctes,
    SyntaqliteBool with_recursive,
    SyntaqliteConflictAction conflict_action,
    uint32_t table,
    SyntaqliteIndexHint index_hint,
    SyntaqliteTextSpan index_name,
    uint32_t setlist,
    uint32_t from_clause,
    uint32_t where_clause,
    uint32_t orderby,
    uint32_t limit_clause,
    uint32_t returning
) {
    return synq_parse_build(ctx,
        &(SyntaqliteUpdateStmt){
            .tag = SYNTAQLITE_NODE_UPDATE_STMT,
            .with_ctes = with_ctes,
            .with_recursive = with_recursive,
            .conflict_action = conflict_action,
            .table = table,
            .index_hint = index_hint,
            .index_name = index_name,
            .setlist = setlist,
            .from_clause = from_clause,
            .where_clause = where_clause,
            .orderby = orderby,
            .limit_clause = limit_clause,
            .returning = returning
        }, (uint32_t)sizeof(SyntaqliteUpdateStmt));
}

static inline uint32_t synq_parse_insert_stmt(
    SynqParseCtx *ctx,
    uint32_t with_ctes,
    SyntaqliteBool with_recursive,
    SyntaqliteInsertKeyword keyword,
    SyntaqliteConflictAction conflict_action,
    uint32_t table,
    uint32_t columns,
    uint32_t source,
    uint32_t upsert,
    uint32_t returning
) {
    return synq_parse_build(ctx,
        &(SyntaqliteInsertStmt){
            .tag = SYNTAQLITE_NODE_INSERT_STMT,
            .with_ctes = with_ctes,
            .with_recursive = with_recursive,
            .keyword = keyword,
            .conflict_action = conflict_action,
            .table = table,
            .columns = columns,
            .source = source,
            .upsert = upsert,
            .returning = returning
        }, (uint32_t)sizeof(SyntaqliteInsertStmt));
}

static inline uint32_t synq_parse_binary_expr(
    SynqParseCtx *ctx,
    SyntaqliteBinaryOp op,
    uint32_t left,
    uint32_t right
) {
    return synq_parse_build(ctx,
        &(SyntaqliteBinaryExpr){
            .tag = SYNTAQLITE_NODE_BINARY_EXPR,
            .op = op,
            .left = left,
            .right = right
        }, (uint32_t)sizeof(SyntaqliteBinaryExpr));
}

static inline uint32_t synq_parse_unary_expr(
    SynqParseCtx *ctx,
    SyntaqliteUnaryOp op,
    uint32_t operand
) {
    return synq_parse_build(ctx,
        &(SyntaqliteUnaryExpr){
            .tag = SYNTAQLITE_NODE_UNARY_EXPR,
            .op = op,
            .operand = operand
        }, (uint32_t)sizeof(SyntaqliteUnaryExpr));
}

static inline uint32_t synq_parse_literal(
    SynqParseCtx *ctx,
    SyntaqliteLiteralType literal_type,
    SyntaqliteTextSpan source
) {
    return synq_parse_build(ctx,
        &(SyntaqliteLiteral){
            .tag = SYNTAQLITE_NODE_LITERAL,
            .literal_type = literal_type,
            .source = source
        }, (uint32_t)sizeof(SyntaqliteLiteral));
}

static inline uint32_t synq_parse_paren_expr(SynqParseCtx *ctx, uint32_t expr) {
    return synq_parse_build(ctx,
        &(SyntaqliteParenExpr){
            .tag = SYNTAQLITE_NODE_PAREN_EXPR,
            .expr = expr
        }, (uint32_t)sizeof(SyntaqliteParenExpr));
}

static inline uint32_t synq_parse_ident_name(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan source
) {
    return synq_parse_build(ctx,
        &(SyntaqliteIdentName){
            .tag = SYNTAQLITE_NODE_IDENT_NAME,
            .source = source
        }, (uint32_t)sizeof(SyntaqliteIdentName));
}

static inline uint32_t synq_parse_error(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan source
) {
    return synq_parse_build(ctx,
        &(SyntaqliteError){
            .tag = SYNTAQLITE_NODE_ERROR,
            .source = source
        }, (uint32_t)sizeof(SyntaqliteError));
}

static inline uint32_t synq_parse_row_value(SynqParseCtx *ctx, uint32_t items) {
    return synq_parse_build(ctx,
        &(SyntaqliteRowValue){
            .tag = SYNTAQLITE_NODE_ROW_VALUE,
            .items = items
        }, (uint32_t)sizeof(SyntaqliteRowValue));
}

static inline uint32_t synq_parse_function_call(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan func_name,
    SyntaqliteFunctionCallFlags flags,
    uint32_t args,
    uint32_t filter_clause,
    uint32_t over_clause
) {
    return synq_parse_build(ctx,
        &(SyntaqliteFunctionCall){
            .tag = SYNTAQLITE_NODE_FUNCTION_CALL,
            .func_name = func_name,
            .flags = flags,
            .args = args,
            .filter_clause = filter_clause,
            .over_clause = over_clause
        }, (uint32_t)sizeof(SyntaqliteFunctionCall));
}

static inline uint32_t synq_parse_variable(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan source
) {
    return synq_parse_build(ctx,
        &(SyntaqliteVariable){
            .tag = SYNTAQLITE_NODE_VARIABLE,
            .source = source
        }, (uint32_t)sizeof(SyntaqliteVariable));
}

static inline uint32_t synq_parse_collate_expr(
    SynqParseCtx *ctx,
    uint32_t expr,
    SyntaqliteTextSpan collation
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCollateExpr){
            .tag = SYNTAQLITE_NODE_COLLATE_EXPR,
            .expr = expr,
            .collation = collation
        }, (uint32_t)sizeof(SyntaqliteCollateExpr));
}

static inline uint32_t synq_parse_raise_expr(
    SynqParseCtx *ctx,
    SyntaqliteRaiseType raise_type,
    uint32_t error_message
) {
    return synq_parse_build(ctx,
        &(SyntaqliteRaiseExpr){
            .tag = SYNTAQLITE_NODE_RAISE_EXPR,
            .raise_type = raise_type,
            .error_message = error_message
        }, (uint32_t)sizeof(SyntaqliteRaiseExpr));
}

static inline uint32_t synq_parse_qualified_name(
    SynqParseCtx *ctx,
    uint32_t object_name,
    uint32_t schema
) {
    return synq_parse_build(ctx,
        &(SyntaqliteQualifiedName){
            .tag = SYNTAQLITE_NODE_QUALIFIED_NAME,
            .object_name = object_name,
            .schema = schema
        }, (uint32_t)sizeof(SyntaqliteQualifiedName));
}

static inline uint32_t synq_parse_drop_stmt(
    SynqParseCtx *ctx,
    SyntaqliteDropObjectType object_type,
    SyntaqliteBool if_exists,
    uint32_t target
) {
    return synq_parse_build(ctx,
        &(SyntaqliteDropStmt){
            .tag = SYNTAQLITE_NODE_DROP_STMT,
            .object_type = object_type,
            .if_exists = if_exists,
            .target = target
        }, (uint32_t)sizeof(SyntaqliteDropStmt));
}

static inline uint32_t synq_parse_alter_table_stmt(
    SynqParseCtx *ctx,
    SyntaqliteAlterOp op,
    SyntaqliteBool has_column_kw,
    uint32_t target,
    uint32_t new_name,
    uint32_t old_name,
    uint32_t column
) {
    return synq_parse_build(ctx,
        &(SyntaqliteAlterTableStmt){
            .tag = SYNTAQLITE_NODE_ALTER_TABLE_STMT,
            .op = op,
            .has_column_kw = has_column_kw,
            .target = target,
            .new_name = new_name,
            .old_name = old_name,
            .column = column
        }, (uint32_t)sizeof(SyntaqliteAlterTableStmt));
}

static inline uint32_t synq_parse_transaction_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTransactionOp op,
    SyntaqliteTransactionType trans_type,
    SyntaqliteBool has_transaction,
    SyntaqliteTextSpan name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteTransactionStmt){
            .tag = SYNTAQLITE_NODE_TRANSACTION_STMT,
            .op = op,
            .trans_type = trans_type,
            .has_transaction = has_transaction,
            .name = name
        }, (uint32_t)sizeof(SyntaqliteTransactionStmt));
}

static inline uint32_t synq_parse_savepoint_stmt(
    SynqParseCtx *ctx,
    SyntaqliteSavepointOp op,
    uint32_t savepoint_name,
    SyntaqliteBool has_savepoint,
    SyntaqliteBool has_transaction,
    SyntaqliteTextSpan transaction_name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteSavepointStmt){
            .tag = SYNTAQLITE_NODE_SAVEPOINT_STMT,
            .op = op,
            .savepoint_name = savepoint_name,
            .has_savepoint = has_savepoint,
            .has_transaction = has_transaction,
            .transaction_name = transaction_name
        }, (uint32_t)sizeof(SyntaqliteSavepointStmt));
}

static inline uint32_t synq_parse_result_column(
    SynqParseCtx *ctx,
    SyntaqliteResultColumnFlags flags,
    uint32_t alias,
    SyntaqliteBool alias_as,
    uint32_t expr
) {
    return synq_parse_build(ctx,
        &(SyntaqliteResultColumn){
            .tag = SYNTAQLITE_NODE_RESULT_COLUMN,
            .flags = flags,
            .alias = alias,
            .alias_as = alias_as,
            .expr = expr
        }, (uint32_t)sizeof(SyntaqliteResultColumn));
}

static inline uint32_t synq_parse_select_stmt(
    SynqParseCtx *ctx,
    SyntaqliteSelectStmtFlags flags,
    uint32_t columns,
    uint32_t from_clause,
    uint32_t where_clause,
    uint32_t groupby,
    uint32_t having,
    uint32_t orderby,
    uint32_t limit_clause,
    uint32_t window_clause
) {
    return synq_parse_build(ctx,
        &(SyntaqliteSelectStmt){
            .tag = SYNTAQLITE_NODE_SELECT_STMT,
            .flags = flags,
            .columns = columns,
            .from_clause = from_clause,
            .where_clause = where_clause,
            .groupby = groupby,
            .having = having,
            .orderby = orderby,
            .limit_clause = limit_clause,
            .window_clause = window_clause
        }, (uint32_t)sizeof(SyntaqliteSelectStmt));
}

static inline uint32_t synq_parse_ordering_term(
    SynqParseCtx *ctx,
    uint32_t expr,
    SyntaqliteSortOrder sort_order,
    SyntaqliteNullsOrder nulls_order
) {
    return synq_parse_build(ctx,
        &(SyntaqliteOrderingTerm){
            .tag = SYNTAQLITE_NODE_ORDERING_TERM,
            .expr = expr,
            .sort_order = sort_order,
            .nulls_order = nulls_order
        }, (uint32_t)sizeof(SyntaqliteOrderingTerm));
}

static inline uint32_t synq_parse_limit_clause(
    SynqParseCtx *ctx,
    uint32_t limit,
    uint32_t offset,
    SyntaqliteBool comma_form
) {
    return synq_parse_build(ctx,
        &(SyntaqliteLimitClause){
            .tag = SYNTAQLITE_NODE_LIMIT_CLAUSE,
            .limit = limit,
            .offset = offset,
            .comma_form = comma_form
        }, (uint32_t)sizeof(SyntaqliteLimitClause));
}

static inline uint32_t synq_parse_join_modifier(
    SynqParseCtx *ctx,
    SyntaqliteJoinModifierKind kind
) {
    return synq_parse_build(ctx,
        &(SyntaqliteJoinModifier){
            .tag = SYNTAQLITE_NODE_JOIN_MODIFIER,
            .kind = kind
        }, (uint32_t)sizeof(SyntaqliteJoinModifier));
}

static inline uint32_t synq_parse_table_ref(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan table_name,
    SyntaqliteTextSpan schema,
    SyntaqliteBool has_parens,
    uint32_t alias,
    SyntaqliteBool alias_as,
    uint32_t args,
    SyntaqliteIndexHint index_hint,
    SyntaqliteTextSpan index_name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteTableRef){
            .tag = SYNTAQLITE_NODE_TABLE_REF,
            .table_name = table_name,
            .schema = schema,
            .has_parens = has_parens,
            .alias = alias,
            .alias_as = alias_as,
            .args = args,
            .index_hint = index_hint,
            .index_name = index_name
        }, (uint32_t)sizeof(SyntaqliteTableRef));
}

static inline uint32_t synq_parse_subquery_table_source(
    SynqParseCtx *ctx,
    uint32_t select,
    uint32_t alias,
    SyntaqliteBool alias_as
) {
    return synq_parse_build(ctx,
        &(SyntaqliteSubqueryTableSource){
            .tag = SYNTAQLITE_NODE_SUBQUERY_TABLE_SOURCE,
            .select = select,
            .alias = alias,
            .alias_as = alias_as
        }, (uint32_t)sizeof(SyntaqliteSubqueryTableSource));
}

static inline uint32_t synq_parse_paren_table_source(
    SynqParseCtx *ctx,
    uint32_t source,
    uint32_t alias,
    SyntaqliteBool alias_as
) {
    return synq_parse_build(ctx,
        &(SyntaqliteParenTableSource){
            .tag = SYNTAQLITE_NODE_PAREN_TABLE_SOURCE,
            .source = source,
            .alias = alias,
            .alias_as = alias_as
        }, (uint32_t)sizeof(SyntaqliteParenTableSource));
}

static inline uint32_t synq_parse_join_clause(
    SynqParseCtx *ctx,
    SyntaqliteJoinType join_type,
    uint32_t modifiers,
    uint32_t left,
    uint32_t right,
    uint32_t on_expr,
    uint32_t using_columns
) {
    return synq_parse_build(ctx,
        &(SyntaqliteJoinClause){
            .tag = SYNTAQLITE_NODE_JOIN_CLAUSE,
            .join_type = join_type,
            .modifiers = modifiers,
            .left = left,
            .right = right,
            .on_expr = on_expr,
            .using_columns = using_columns
        }, (uint32_t)sizeof(SyntaqliteJoinClause));
}

static inline uint32_t synq_parse_join_prefix(
    SynqParseCtx *ctx,
    uint32_t source,
    SyntaqliteJoinType join_type,
    uint32_t modifiers
) {
    return synq_parse_build(ctx,
        &(SyntaqliteJoinPrefix){
            .tag = SYNTAQLITE_NODE_JOIN_PREFIX,
            .source = source,
            .join_type = join_type,
            .modifiers = modifiers
        }, (uint32_t)sizeof(SyntaqliteJoinPrefix));
}

static inline uint32_t synq_parse_trigger_event(
    SynqParseCtx *ctx,
    SyntaqliteTriggerEventType event_type,
    uint32_t columns
) {
    return synq_parse_build(ctx,
        &(SyntaqliteTriggerEvent){
            .tag = SYNTAQLITE_NODE_TRIGGER_EVENT,
            .event_type = event_type,
            .columns = columns
        }, (uint32_t)sizeof(SyntaqliteTriggerEvent));
}

static inline uint32_t synq_parse_create_trigger_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan trigger_name,
    SyntaqliteTextSpan schema,
    SyntaqliteTemporaryQualifier temporary,
    SyntaqliteBool if_not_exists,
    SyntaqliteTriggerTiming timing,
    SyntaqliteBool for_each_row,
    uint32_t event,
    uint32_t table,
    uint32_t when_expr,
    uint32_t body
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreateTriggerStmt){
            .tag = SYNTAQLITE_NODE_CREATE_TRIGGER_STMT,
            .trigger_name = trigger_name,
            .schema = schema,
            .temporary = temporary,
            .if_not_exists = if_not_exists,
            .timing = timing,
            .for_each_row = for_each_row,
            .event = event,
            .table = table,
            .when_expr = when_expr,
            .body = body
        }, (uint32_t)sizeof(SyntaqliteCreateTriggerStmt));
}

static inline uint32_t synq_parse_create_virtual_table_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan table_name,
    SyntaqliteTextSpan schema,
    SyntaqliteTextSpan module_name,
    SyntaqliteBool if_not_exists,
    SyntaqliteBool has_module_args,
    SyntaqliteTextSpan module_args
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreateVirtualTableStmt){
            .tag = SYNTAQLITE_NODE_CREATE_VIRTUAL_TABLE_STMT,
            .table_name = table_name,
            .schema = schema,
            .module_name = module_name,
            .if_not_exists = if_not_exists,
            .has_module_args = has_module_args,
            .module_args = module_args
        }, (uint32_t)sizeof(SyntaqliteCreateVirtualTableStmt));
}

static inline uint32_t synq_parse_pragma_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan pragma_name,
    SyntaqliteTextSpan schema,
    SyntaqliteTextSpan value,
    SyntaqlitePragmaForm pragma_form
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePragmaStmt){
            .tag = SYNTAQLITE_NODE_PRAGMA_STMT,
            .pragma_name = pragma_name,
            .schema = schema,
            .value = value,
            .pragma_form = pragma_form
        }, (uint32_t)sizeof(SyntaqlitePragmaStmt));
}

static inline uint32_t synq_parse_analyze_or_reindex_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan target_name,
    SyntaqliteTextSpan schema,
    SyntaqliteAnalyzeOrReindexOp kind
) {
    return synq_parse_build(ctx,
        &(SyntaqliteAnalyzeOrReindexStmt){
            .tag = SYNTAQLITE_NODE_ANALYZE_OR_REINDEX_STMT,
            .target_name = target_name,
            .schema = schema,
            .kind = kind
        }, (uint32_t)sizeof(SyntaqliteAnalyzeOrReindexStmt));
}

static inline uint32_t synq_parse_attach_stmt(
    SynqParseCtx *ctx,
    SyntaqliteBool has_database,
    uint32_t filename,
    uint32_t db_name,
    uint32_t key
) {
    return synq_parse_build(ctx,
        &(SyntaqliteAttachStmt){
            .tag = SYNTAQLITE_NODE_ATTACH_STMT,
            .has_database = has_database,
            .filename = filename,
            .db_name = db_name,
            .key = key
        }, (uint32_t)sizeof(SyntaqliteAttachStmt));
}

static inline uint32_t synq_parse_detach_stmt(
    SynqParseCtx *ctx,
    SyntaqliteBool has_database,
    uint32_t db_name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteDetachStmt){
            .tag = SYNTAQLITE_NODE_DETACH_STMT,
            .has_database = has_database,
            .db_name = db_name
        }, (uint32_t)sizeof(SyntaqliteDetachStmt));
}

static inline uint32_t synq_parse_vacuum_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan schema,
    uint32_t filename
) {
    return synq_parse_build(ctx,
        &(SyntaqliteVacuumStmt){
            .tag = SYNTAQLITE_NODE_VACUUM_STMT,
            .schema = schema,
            .filename = filename
        }, (uint32_t)sizeof(SyntaqliteVacuumStmt));
}

static inline uint32_t synq_parse_explain_stmt(
    SynqParseCtx *ctx,
    SyntaqliteExplainMode explain_mode,
    uint32_t stmt
) {
    return synq_parse_build(ctx,
        &(SyntaqliteExplainStmt){
            .tag = SYNTAQLITE_NODE_EXPLAIN_STMT,
            .explain_mode = explain_mode,
            .stmt = stmt
        }, (uint32_t)sizeof(SyntaqliteExplainStmt));
}

static inline uint32_t synq_parse_create_index_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan index_name,
    SyntaqliteTextSpan schema,
    SyntaqliteTextSpan table_name,
    SyntaqliteBool is_unique,
    SyntaqliteBool if_not_exists,
    uint32_t columns,
    uint32_t where_clause
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreateIndexStmt){
            .tag = SYNTAQLITE_NODE_CREATE_INDEX_STMT,
            .index_name = index_name,
            .schema = schema,
            .table_name = table_name,
            .is_unique = is_unique,
            .if_not_exists = if_not_exists,
            .columns = columns,
            .where_clause = where_clause
        }, (uint32_t)sizeof(SyntaqliteCreateIndexStmt));
}

static inline uint32_t synq_parse_create_view_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan view_name,
    SyntaqliteTextSpan schema,
    SyntaqliteTemporaryQualifier temporary,
    SyntaqliteBool if_not_exists,
    uint32_t column_names,
    uint32_t select
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreateViewStmt){
            .tag = SYNTAQLITE_NODE_CREATE_VIEW_STMT,
            .view_name = view_name,
            .schema = schema,
            .temporary = temporary,
            .if_not_exists = if_not_exists,
            .column_names = column_names,
            .select = select
        }, (uint32_t)sizeof(SyntaqliteCreateViewStmt));
}

static inline uint32_t synq_parse_values_clause(
    SynqParseCtx *ctx,
    uint32_t rows
) {
    return synq_parse_build(ctx,
        &(SyntaqliteValuesClause){
            .tag = SYNTAQLITE_NODE_VALUES_CLAUSE,
            .rows = rows
        }, (uint32_t)sizeof(SyntaqliteValuesClause));
}

static inline uint32_t synq_parse_frame_bound(
    SynqParseCtx *ctx,
    SyntaqliteFrameBoundType bound_type,
    uint32_t expr
) {
    return synq_parse_build(ctx,
        &(SyntaqliteFrameBound){
            .tag = SYNTAQLITE_NODE_FRAME_BOUND,
            .bound_type = bound_type,
            .expr = expr
        }, (uint32_t)sizeof(SyntaqliteFrameBound));
}

static inline uint32_t synq_parse_frame_spec(
    SynqParseCtx *ctx,
    SyntaqliteFrameType frame_type,
    SyntaqliteFrameExclude exclude,
    uint32_t start_bound,
    uint32_t end_bound
) {
    return synq_parse_build(ctx,
        &(SyntaqliteFrameSpec){
            .tag = SYNTAQLITE_NODE_FRAME_SPEC,
            .frame_type = frame_type,
            .exclude = exclude,
            .start_bound = start_bound,
            .end_bound = end_bound
        }, (uint32_t)sizeof(SyntaqliteFrameSpec));
}

static inline uint32_t synq_parse_window_def(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan ref_window_name,
    SyntaqliteTextSpan base_window_name,
    uint32_t partition_by,
    uint32_t orderby,
    uint32_t frame
) {
    return synq_parse_build(ctx,
        &(SyntaqliteWindowDef){
            .tag = SYNTAQLITE_NODE_WINDOW_DEF,
            .ref_window_name = ref_window_name,
            .base_window_name = base_window_name,
            .partition_by = partition_by,
            .orderby = orderby,
            .frame = frame
        }, (uint32_t)sizeof(SyntaqliteWindowDef));
}

static inline uint32_t synq_parse_named_window_def(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan window_name,
    uint32_t window_def
) {
    return synq_parse_build(ctx,
        &(SyntaqliteNamedWindowDef){
            .tag = SYNTAQLITE_NODE_NAMED_WINDOW_DEF,
            .window_name = window_name,
            .window_def = window_def
        }, (uint32_t)sizeof(SyntaqliteNamedWindowDef));
}

static inline uint32_t synq_parse_filter_over(
    SynqParseCtx *ctx,
    uint32_t filter_expr,
    uint32_t over_def,
    SyntaqliteTextSpan over_name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteFilterOver){
            .tag = SYNTAQLITE_NODE_FILTER_OVER,
            .filter_expr = filter_expr,
            .over_def = over_def,
            .over_name = over_name
        }, (uint32_t)sizeof(SyntaqliteFilterOver));
}

static inline uint32_t synq_parse_perfetto_arg_def(
    SynqParseCtx *ctx,
    uint32_t arg_name,
    SyntaqliteTextSpan arg_type,
    SyntaqliteBool is_variadic
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoArgDef){
            .tag = SYNTAQLITE_NODE_PERFETTO_ARG_DEF,
            .arg_name = arg_name,
            .arg_type = arg_type,
            .is_variadic = is_variadic
        }, (uint32_t)sizeof(SyntaqlitePerfettoArgDef));
}

static inline uint32_t synq_parse_perfetto_macro_arg(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan arg_name,
    SyntaqliteTextSpan arg_type
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoMacroArg){
            .tag = SYNTAQLITE_NODE_PERFETTO_MACRO_ARG,
            .arg_name = arg_name,
            .arg_type = arg_type
        }, (uint32_t)sizeof(SyntaqlitePerfettoMacroArg));
}

static inline uint32_t synq_parse_perfetto_indexed_column(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan column_name
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoIndexedColumn){
            .tag = SYNTAQLITE_NODE_PERFETTO_INDEXED_COLUMN,
            .column_name = column_name
        }, (uint32_t)sizeof(SyntaqlitePerfettoIndexedColumn));
}

static inline uint32_t synq_parse_perfetto_return_type(
    SynqParseCtx *ctx,
    SyntaqlitePerfettoReturnKind kind,
    SyntaqliteTextSpan scalar_type,
    uint32_t table_columns
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoReturnType){
            .tag = SYNTAQLITE_NODE_PERFETTO_RETURN_TYPE,
            .kind = kind,
            .scalar_type = scalar_type,
            .table_columns = table_columns
        }, (uint32_t)sizeof(SyntaqlitePerfettoReturnType));
}

static inline uint32_t synq_parse_perfetto_table_impl(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan name
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoTableImpl){
            .tag = SYNTAQLITE_NODE_PERFETTO_TABLE_IMPL,
            .name = name
        }, (uint32_t)sizeof(SyntaqlitePerfettoTableImpl));
}

static inline uint32_t synq_parse_create_perfetto_table_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan table_name,
    SyntaqliteBool or_replace,
    uint32_t table_impl,
    uint32_t schema,
    uint32_t select,
    SyntaqliteTextSpan select_span,
    uint32_t pipeline
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreatePerfettoTableStmt){
            .tag = SYNTAQLITE_NODE_CREATE_PERFETTO_TABLE_STMT,
            .table_name = table_name,
            .or_replace = or_replace,
            .table_impl = table_impl,
            .schema = schema,
            .select = select,
            .select_span = select_span,
            .pipeline = pipeline
        }, (uint32_t)sizeof(SyntaqliteCreatePerfettoTableStmt));
}

static inline uint32_t synq_parse_create_perfetto_view_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan view_name,
    SyntaqliteBool or_replace,
    uint32_t schema,
    uint32_t select,
    SyntaqliteTextSpan select_span
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreatePerfettoViewStmt){
            .tag = SYNTAQLITE_NODE_CREATE_PERFETTO_VIEW_STMT,
            .view_name = view_name,
            .or_replace = or_replace,
            .schema = schema,
            .select = select,
            .select_span = select_span
        }, (uint32_t)sizeof(SyntaqliteCreatePerfettoViewStmt));
}

static inline uint32_t synq_parse_create_perfetto_function_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan function_name,
    SyntaqliteBool or_replace,
    uint32_t args,
    uint32_t return_type,
    uint32_t select,
    SyntaqliteTextSpan select_span
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreatePerfettoFunctionStmt){
            .tag = SYNTAQLITE_NODE_CREATE_PERFETTO_FUNCTION_STMT,
            .function_name = function_name,
            .or_replace = or_replace,
            .args = args,
            .return_type = return_type,
            .select = select,
            .select_span = select_span
        }, (uint32_t)sizeof(SyntaqliteCreatePerfettoFunctionStmt));
}

static inline uint32_t synq_parse_create_perfetto_delegating_function_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan function_name,
    SyntaqliteBool or_replace,
    uint32_t args,
    uint32_t return_type,
    SyntaqliteTextSpan delegate_to
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreatePerfettoDelegatingFunctionStmt){
            .tag = SYNTAQLITE_NODE_CREATE_PERFETTO_DELEGATING_FUNCTION_STMT,
            .function_name = function_name,
            .or_replace = or_replace,
            .args = args,
            .return_type = return_type,
            .delegate_to = delegate_to
        }, (uint32_t)sizeof(SyntaqliteCreatePerfettoDelegatingFunctionStmt));
}

static inline uint32_t synq_parse_create_perfetto_index_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan index_name,
    SyntaqliteBool or_replace,
    SyntaqliteTextSpan table_name,
    uint32_t columns
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreatePerfettoIndexStmt){
            .tag = SYNTAQLITE_NODE_CREATE_PERFETTO_INDEX_STMT,
            .index_name = index_name,
            .or_replace = or_replace,
            .table_name = table_name,
            .columns = columns
        }, (uint32_t)sizeof(SyntaqliteCreatePerfettoIndexStmt));
}

static inline uint32_t synq_parse_create_perfetto_macro_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan macro_name,
    SyntaqliteBool or_replace,
    SyntaqliteTextSpan return_type,
    SyntaqliteTextSpan body,
    uint32_t args
) {
    return synq_parse_build(ctx,
        &(SyntaqliteCreatePerfettoMacroStmt){
            .tag = SYNTAQLITE_NODE_CREATE_PERFETTO_MACRO_STMT,
            .macro_name = macro_name,
            .or_replace = or_replace,
            .return_type = return_type,
            .body = body,
            .args = args
        }, (uint32_t)sizeof(SyntaqliteCreatePerfettoMacroStmt));
}

static inline uint32_t synq_parse_include_perfetto_module_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan module_name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteIncludePerfettoModuleStmt){
            .tag = SYNTAQLITE_NODE_INCLUDE_PERFETTO_MODULE_STMT,
            .module_name = module_name
        }, (uint32_t)sizeof(SyntaqliteIncludePerfettoModuleStmt));
}

static inline uint32_t synq_parse_drop_perfetto_index_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan index_name,
    SyntaqliteTextSpan table_name
) {
    return synq_parse_build(ctx,
        &(SyntaqliteDropPerfettoIndexStmt){
            .tag = SYNTAQLITE_NODE_DROP_PERFETTO_INDEX_STMT,
            .index_name = index_name,
            .table_name = table_name
        }, (uint32_t)sizeof(SyntaqliteDropPerfettoIndexStmt));
}

static inline uint32_t synq_parse_perfetto_pragma_stmt(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan name,
    uint32_t value
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoPragmaStmt){
            .tag = SYNTAQLITE_NODE_PERFETTO_PRAGMA_STMT,
            .name = name,
            .value = value
        }, (uint32_t)sizeof(SyntaqlitePerfettoPragmaStmt));
}

static inline uint32_t synq_parse_perfetto_pipe_source(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan table_name,
    SyntaqliteTextSpan schema,
    uint32_t select,
    uint32_t alias,
    SyntaqliteBool alias_as
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoPipeSource){
            .tag = SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE,
            .table_name = table_name,
            .schema = schema,
            .select = select,
            .alias = alias,
            .alias_as = alias_as
        }, (uint32_t)sizeof(SyntaqlitePerfettoPipeSource));
}

static inline uint32_t synq_parse_perfetto_tree_aggregate(
    SynqParseCtx *ctx,
    uint32_t expr,
    SyntaqliteTextSpan name
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoTreeAggregate){
            .tag = SYNTAQLITE_NODE_PERFETTO_TREE_AGGREGATE,
            .expr = expr,
            .name = name
        }, (uint32_t)sizeof(SyntaqlitePerfettoTreeAggregate));
}

static inline uint32_t synq_parse_perfetto_tree_accumulate(
    SynqParseCtx *ctx,
    SyntaqlitePerfettoTreeDirection direction,
    uint32_t aggregates
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoTreeAccumulate){
            .tag = SYNTAQLITE_NODE_PERFETTO_TREE_ACCUMULATE,
            .direction = direction,
            .aggregates = aggregates
        }, (uint32_t)sizeof(SyntaqlitePerfettoTreeAccumulate));
}

static inline uint32_t synq_parse_perfetto_pipe_column(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan qualifier,
    SyntaqliteTextSpan name,
    SyntaqliteTextSpan alias
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoPipeColumn){
            .tag = SYNTAQLITE_NODE_PERFETTO_PIPE_COLUMN,
            .qualifier = qualifier,
            .name = name,
            .alias = alias
        }, (uint32_t)sizeof(SyntaqlitePerfettoPipeColumn));
}

static inline uint32_t synq_parse_perfetto_pipe_select(
    SynqParseCtx *ctx,
    uint32_t columns
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoPipeSelect){
            .tag = SYNTAQLITE_NODE_PERFETTO_PIPE_SELECT,
            .columns = columns
        }, (uint32_t)sizeof(SyntaqlitePerfettoPipeSelect));
}

static inline uint32_t synq_parse_perfetto_per_column(
    SynqParseCtx *ctx,
    SyntaqliteTextSpan name
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoPerColumn){
            .tag = SYNTAQLITE_NODE_PERFETTO_PER_COLUMN,
            .name = name
        }, (uint32_t)sizeof(SyntaqlitePerfettoPerColumn));
}

static inline uint32_t synq_parse_perfetto_interval_intersection(
    SynqParseCtx *ctx,
    uint32_t operands,
    uint32_t per
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoIntervalIntersection){
            .tag = SYNTAQLITE_NODE_PERFETTO_INTERVAL_INTERSECTION,
            .operands = operands,
            .per = per
        }, (uint32_t)sizeof(SyntaqlitePerfettoIntervalIntersection));
}

static inline uint32_t synq_parse_perfetto_pipeline(
    SynqParseCtx *ctx,
    uint32_t from,
    uint32_t intersection,
    uint32_t stages
) {
    return synq_parse_build(ctx,
        &(SyntaqlitePerfettoPipeline){
            .tag = SYNTAQLITE_NODE_PERFETTO_PIPELINE,
            .from = from,
            .intersection = intersection,
            .stages = stages
        }, (uint32_t)sizeof(SyntaqlitePerfettoPipeline));
}

static inline uint32_t synq_parse_case_when_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_CASE_WHEN_LIST, list_id, child);
}

static inline uint32_t synq_parse_foreign_key_option_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_FOREIGN_KEY_OPTION_LIST, list_id, child);
}

static inline uint32_t synq_parse_column_constraint_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_COLUMN_CONSTRAINT_LIST, list_id, child);
}

static inline uint32_t synq_parse_column_def_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_COLUMN_DEF_LIST, list_id, child);
}

static inline uint32_t synq_parse_table_constraint_group(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_TABLE_CONSTRAINT_GROUP, list_id, child);
}

static inline uint32_t synq_parse_table_constraint_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_TABLE_CONSTRAINT_LIST, list_id, child);
}

static inline uint32_t synq_parse_cte_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_CTE_LIST, list_id, child);
}

static inline uint32_t synq_parse_upsert_clause_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_prepend(ctx, SYNTAQLITE_NODE_UPSERT_CLAUSE_LIST, list_id, child);
}

static inline uint32_t synq_parse_set_clause_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_SET_CLAUSE_LIST, list_id, child);
}

static inline uint32_t synq_parse_expr_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_EXPR_LIST, list_id, child);
}

static inline uint32_t synq_parse_result_column_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_RESULT_COLUMN_LIST, list_id, child);
}

static inline uint32_t synq_parse_order_by_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_ORDER_BY_LIST, list_id, child);
}

static inline uint32_t synq_parse_join_modifier_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_JOIN_MODIFIER_LIST, list_id, child);
}

static inline uint32_t synq_parse_trigger_cmd_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_TRIGGER_CMD_LIST, list_id, child);
}

static inline uint32_t synq_parse_values_row_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_VALUES_ROW_LIST, list_id, child);
}

static inline uint32_t synq_parse_window_def_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_WINDOW_DEF_LIST, list_id, child);
}

static inline uint32_t synq_parse_named_window_def_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_NAMED_WINDOW_DEF_LIST, list_id, child);
}

static inline uint32_t synq_parse_perfetto_arg_def_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_PERFETTO_ARG_DEF_LIST, list_id, child);
}

static inline uint32_t synq_parse_perfetto_macro_arg_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_PERFETTO_MACRO_ARG_LIST, list_id, child);
}

static inline uint32_t synq_parse_perfetto_indexed_column_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_PERFETTO_INDEXED_COLUMN_LIST, list_id, child);
}

static inline uint32_t synq_parse_perfetto_tree_aggregate_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_PERFETTO_TREE_AGGREGATE_LIST, list_id, child);
}

static inline uint32_t synq_parse_perfetto_pipe_column_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_PERFETTO_PIPE_COLUMN_LIST, list_id, child);
}

static inline uint32_t synq_parse_perfetto_pipe_stage_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_PERFETTO_PIPE_STAGE_LIST, list_id, child);
}

static inline uint32_t synq_parse_perfetto_pipe_source_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_PERFETTO_PIPE_SOURCE_LIST, list_id, child);
}

static inline uint32_t synq_parse_perfetto_per_column_list(
    SynqParseCtx *ctx,
    uint32_t list_id,
    uint32_t child
) {
    return synq_parse_list_append(ctx, SYNTAQLITE_NODE_PERFETTO_PER_COLUMN_LIST, list_id, child);
}

#ifdef __cplusplus
}
#endif


#endif  /* SYNTAQLITE_PERFETTO_DIALECT_BUILDER_H */
/* ======== end: csrc/dialect_builder.h ======== */

/* ======== begin: csrc/sqlite_parse.c ======== */
/*
** 2000-05-29
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** Driver template for the LEMON parser generator.
**
** The "lemon" program processes an LALR(1) input grammar file, then uses
** this template to construct a parser.  The "lemon" program inserts text
** at each "%%" line.  Also, any "P-a-r-s-e" identifier prefix (without the
** interstitial "-" characters) contained in this template is changed into
** the value of the %name directive from the grammar.  Otherwise, the content
** of this template is copied straight through into the generate parser
** source file.
**
** The following is the concatenation of all %include directives from the
** input grammar file:
*/
/************ Begin %include sections from the grammar ************************/
#include <string.h>
#include <limits.h>


// Parser stack realloc/free macros. These expand at the Lemon call site
// where the parser struct is in scope, routing through pCtx->mem.
// YYREALLOC is called in yyGrowStack (parser variable: p).
// YYFREE is called in ParseFinalize (parser variable: pParser).
#define synq_stack_realloc(ptr, sz) (p->pCtx->mem.xRealloc((ptr), (sz)))
#define synq_stack_free(ptr)        (pParser->pCtx->mem.xFree((ptr)))

/* BEGIN GRAMMAR_TYPES */
// Grammar-specific struct types for multi-valued grammar nonterminals.
// These are used by Lemon-generated parser actions to bundle multiple
// values through a single nonterminal reduction.

// columnname: passes name span + typetoken span from column definition.
typedef struct SynqColumnNameValue {
  uint32_t name;
  SyntaqliteTextSpan typetoken;
} SynqColumnNameValue;

// conslist: completed comma-separated groups and the group still being built.
typedef struct SynqConstraintGroups {
  uint32_t list;
  uint32_t group;
} SynqConstraintGroups;

// trans_opt: whether the optional TRANSACTION keyword was written, plus the
// optional name that may follow it.
typedef struct SynqTransOptValue {
  int has_transaction;
  SynqParseToken name;
} SynqTransOptValue;

// as: an optional alias plus whether the AS keyword was authored.  Without
// the second field `SELECT a x` and `SELECT a AS x` are indistinguishable.
typedef struct SynqAliasValue {
  uint32_t name;
  int has_as;
} SynqAliasValue;

// defer_subclause: DEFERRABLE / NOT DEFERRABLE plus the INITIALLY mode.
typedef struct SynqDeferValue {
  SyntaqliteDeferrable deferrable;
  SyntaqliteInitialDeferMode initial;
} SynqDeferValue;

// on_using: ON expr / USING column-list discriminator.
typedef struct SynqOnUsingValue {
  uint32_t on_expr;
  uint32_t using_cols;
} SynqOnUsingValue;

// with: recursive flag + CTE list node ID.
typedef struct SynqWithValue {
  uint32_t cte_list;
  int is_recursive;
} SynqWithValue;

// where_opt_ret: WHERE expr + optional RETURNING column list.
typedef struct SynqWhereRetValue {
  uint32_t where_expr;
  uint32_t returning;
} SynqWhereRetValue;

// upsert: accumulated ON CONFLICT clauses + optional RETURNING column list.
typedef struct SynqUpsertValue {
  uint32_t clauses;
  uint32_t returning;
} SynqUpsertValue;

// Keep the authored INSERT/REPLACE form separate from conflict semantics.
typedef struct SynqInsertCmdValue {
  SyntaqliteInsertKeyword keyword;
  SyntaqliteConflictAction conflict_action;
} SynqInsertCmdValue;

// Keep the authored modifier sequence alongside its semantic join type.
typedef struct SynqJoinOpValue {
  SyntaqliteJoinType join_type;
  uint32_t modifiers;
} SynqJoinOpValue;

// paren_exprlist: optional `LP exprlist RP` tail. Tracks whether the
// parens were present so callers can distinguish `foo` (has_parens=0)
// from `foo()` (has_parens=1, args=NULL_NODE) — relevant for table /
// table-valued function references where the two forms are distinct
// productions in the SQLite grammar.
typedef struct SynqParenExprlistValue {
  uint32_t args;
  SyntaqliteBool has_parens;
} SynqParenExprlistValue;
/* END GRAMMAR_TYPES */

#define YYPARSEFREENEVERNULL 1

// The ID fallback lets `GENERATED ALWAYS` be absorbed into a preceding type
// name. Trim it back off as sqlite3AddColumn does, reporting whether it was
// there so the keywords can still be emitted.
static inline int synq_trim_generated_always(SynqParseToken* t) {
  if (t->z == NULL || t->n < 16) {
    return 0;
  }
  if (SYNQ_STRNCASECMP(t->z + (t->n - 6), "always", 6) != 0) {
    return 0;
  }
  int n = t->n - 6;
  while (n > 0 && (t->z[n - 1] == ' ' || t->z[n - 1] == '\t' ||
                   t->z[n - 1] == '\n' || t->z[n - 1] == '\r')) {
    n--;
  }
  if (n < 9 || SYNQ_STRNCASECMP(t->z + (n - 9), "generated", 9) != 0) {
    return 0;
  }
  n -= 9;
  while (n > 0 && (t->z[n - 1] == ' ' || t->z[n - 1] == '\t' ||
                   t->z[n - 1] == '\n' || t->z[n - 1] == '\r')) {
    n--;
  }
  t->n = n;
  return 1;
}

// Semantic join type depends on the set of keywords; see sqlite3JoinType().
#define SYNQ_JT_INNER   0x01
#define SYNQ_JT_CROSS   0x02
#define SYNQ_JT_NATURAL 0x04
#define SYNQ_JT_LEFT    0x08
#define SYNQ_JT_RIGHT   0x10
#define SYNQ_JT_OUTER   0x20
#define SYNQ_JT_ERROR   0x40

static inline int synq_append_join_modifier(SynqParseCtx* ctx,
                                             uint32_t* modifiers,
                                             const SynqParseToken* token) {
  static const struct {
    const char* text;
    unsigned char len;
    unsigned char mask;
    SyntaqliteJoinModifierKind kind;
  } keywords[] = {
    {"natural", 7, SYNQ_JT_NATURAL, SYNTAQLITE_JOIN_MODIFIER_KIND_NATURAL},
    {"left", 4, SYNQ_JT_LEFT | SYNQ_JT_OUTER, SYNTAQLITE_JOIN_MODIFIER_KIND_LEFT},
    {"outer", 5, SYNQ_JT_OUTER, SYNTAQLITE_JOIN_MODIFIER_KIND_OUTER},
    {"right", 5, SYNQ_JT_RIGHT | SYNQ_JT_OUTER, SYNTAQLITE_JOIN_MODIFIER_KIND_RIGHT},
    {"full", 4, SYNQ_JT_LEFT | SYNQ_JT_RIGHT | SYNQ_JT_OUTER, SYNTAQLITE_JOIN_MODIFIER_KIND_FULL},
    {"inner", 5, SYNQ_JT_INNER, SYNTAQLITE_JOIN_MODIFIER_KIND_INNER},
    {"cross", 5, SYNQ_JT_INNER | SYNQ_JT_CROSS, SYNTAQLITE_JOIN_MODIFIER_KIND_CROSS},
  };
  if (token == NULL) return 0;
  for (unsigned i = 0; i < sizeof(keywords) / sizeof(keywords[0]); ++i) {
    if (token->n == keywords[i].len &&
        SYNQ_STRNCASECMP(token->z, keywords[i].text, token->n) == 0) {
      uint32_t modifier = synq_parse_join_modifier(ctx, keywords[i].kind);
      *modifiers = synq_parse_join_modifier_list(ctx, *modifiers, modifier);
      return keywords[i].mask;
    }
  }
  return SYNQ_JT_ERROR;
}

// SQLite reports invalid modifier sets before falling back internally.
static inline SyntaqliteJoinType synq_join_type(SynqParseCtx* ctx, int m) {
  if ((m & (SYNQ_JT_INNER | SYNQ_JT_OUTER)) == (SYNQ_JT_INNER | SYNQ_JT_OUTER) ||
      (m & SYNQ_JT_ERROR) != 0 ||
      (m & (SYNQ_JT_OUTER | SYNQ_JT_LEFT | SYNQ_JT_RIGHT)) == SYNQ_JT_OUTER) {
    ctx->error = 1;
    return SYNTAQLITE_JOIN_TYPE_INNER;
  }
  if (m & SYNQ_JT_NATURAL) {
    if (m & SYNQ_JT_CROSS) return SYNTAQLITE_JOIN_TYPE_NATURAL_CROSS;
    if ((m & SYNQ_JT_LEFT) && (m & SYNQ_JT_RIGHT))
      return SYNTAQLITE_JOIN_TYPE_NATURAL_FULL;
    if (m & SYNQ_JT_LEFT) return SYNTAQLITE_JOIN_TYPE_NATURAL_LEFT;
    if (m & SYNQ_JT_RIGHT) return SYNTAQLITE_JOIN_TYPE_NATURAL_RIGHT;
    return SYNTAQLITE_JOIN_TYPE_NATURAL_INNER;
  }
  if (m & SYNQ_JT_CROSS) return SYNTAQLITE_JOIN_TYPE_CROSS;
  if ((m & SYNQ_JT_LEFT) && (m & SYNQ_JT_RIGHT))
    return SYNTAQLITE_JOIN_TYPE_FULL;
  if (m & SYNQ_JT_LEFT) return SYNTAQLITE_JOIN_TYPE_LEFT;
  if (m & SYNQ_JT_RIGHT) return SYNTAQLITE_JOIN_TYPE_RIGHT;
  return SYNTAQLITE_JOIN_TYPE_INNER;
}

static inline SynqJoinOpValue synq_join_operator(SynqParseCtx* ctx,
                                                const SynqParseToken* a,
                                                const SynqParseToken* b,
                                                const SynqParseToken* c) {
  const SynqParseToken* tokens[] = {a, b, c};
  uint32_t modifiers = SYNTAQLITE_NULL_NODE;
  int mask = 0;
  for (unsigned i = 0; i < sizeof(tokens) / sizeof(tokens[0]); ++i) {
    mask |= synq_append_join_modifier(ctx, &modifiers, tokens[i]);
  }
  return (SynqJoinOpValue){synq_join_type(ctx, mask), modifiers};
}

// ON / USING need a left-hand term to join to (build.c
// sqlite3SrcListAppendFromTerm).
static inline void synq_reject_dangling_on_using(SynqParseCtx* pCtx,
                                                 SynqOnUsingValue n) {
  if (n.on_expr != SYNTAQLITE_NULL_NODE || n.using_cols != SYNTAQLITE_NULL_NODE) {
    pCtx->error = 1;
  }
}

// An authored ASC is not the same as no sort order: SQLite treats them
// identically but they are different text, so the AST keeps them apart. NONE
// is zero so a node with no sort order at all gets it by default.
#define SYNQ_SORTORDER_NONE 0

static inline int synq_is_digit(char c) { return c >= '0' && c <= '9'; }

static inline int synq_is_xdigit(char c) {
  return synq_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Port of sqlite3DequoteNumber's separator validation (util.c): every '_' must
// sit between two digits, or two hex digits for an 0x literal.
static inline int synq_qnumber_is_valid(const char* z, uint32_t n) {
  int is_hex = n > 1 && z[0] == '0' && (z[1] == 'x' || z[1] == 'X');
  for (uint32_t i = 0; i < n; i++) {
    if (z[i] != '_') continue;
    if (i == 0 || i + 1 >= n) return 0;
    char prev = z[i - 1], next = z[i + 1];
    if (is_hex) {
      if (!synq_is_xdigit(prev) || !synq_is_xdigit(next)) return 0;
    } else {
      if (!synq_is_digit(prev) || !synq_is_digit(next)) return 0;
    }
  }
  return 1;
}

// Map parser error bookkeeping to a best-effort source span.
static inline SyntaqliteTextSpan synq_error_span(SynqParseCtx* pCtx) {
  if (pCtx->error_offset == 0xFFFFFFFF || pCtx->error_length == 0) {
    return SYNQ_NO_SPAN;
  }
  return (SyntaqliteTextSpan){
      .offset = pCtx->error_offset,
      .length = pCtx->error_length,
      .flags = 0,
  };
}
/**************** End of %include directives **********************************/
/* These constants specify the various numeric values for terminal symbols.
***************** Begin token definitions *************************************/
#ifndef SYNTAQLITE_TK_ABORT
#define SYNTAQLITE_TK_ABORT                           1
#define SYNTAQLITE_TK_ACTION                          2
#define SYNTAQLITE_TK_AFTER                           3
#define SYNTAQLITE_TK_ANALYZE                         4
#define SYNTAQLITE_TK_ASC                             5
#define SYNTAQLITE_TK_ATTACH                          6
#define SYNTAQLITE_TK_BEFORE                          7
#define SYNTAQLITE_TK_BEGIN                           8
#define SYNTAQLITE_TK_BY                              9
#define SYNTAQLITE_TK_CASCADE                        10
#define SYNTAQLITE_TK_CAST                           11
#define SYNTAQLITE_TK_CONFLICT                       12
#define SYNTAQLITE_TK_DATABASE                       13
#define SYNTAQLITE_TK_DEFERRED                       14
#define SYNTAQLITE_TK_DESC                           15
#define SYNTAQLITE_TK_DETACH                         16
#define SYNTAQLITE_TK_EACH                           17
#define SYNTAQLITE_TK_END                            18
#define SYNTAQLITE_TK_EXCLUSIVE                      19
#define SYNTAQLITE_TK_EXPLAIN                        20
#define SYNTAQLITE_TK_FAIL                           21
#define SYNTAQLITE_TK_OR                             22
#define SYNTAQLITE_TK_AND                            23
#define SYNTAQLITE_TK_NOT                            24
#define SYNTAQLITE_TK_IS                             25
#define SYNTAQLITE_TK_ISNOT                          26
#define SYNTAQLITE_TK_MATCH                          27
#define SYNTAQLITE_TK_LIKE_KW                        28
#define SYNTAQLITE_TK_BETWEEN                        29
#define SYNTAQLITE_TK_IN                             30
#define SYNTAQLITE_TK_ISNULL                         31
#define SYNTAQLITE_TK_NOTNULL                        32
#define SYNTAQLITE_TK_NE                             33
#define SYNTAQLITE_TK_EQ                             34
#define SYNTAQLITE_TK_GT                             35
#define SYNTAQLITE_TK_LE                             36
#define SYNTAQLITE_TK_LT                             37
#define SYNTAQLITE_TK_GE                             38
#define SYNTAQLITE_TK_ESCAPE                         39
#define SYNTAQLITE_TK_ID                             40
#define SYNTAQLITE_TK_COLUMNKW                       41
#define SYNTAQLITE_TK_DO                             42
#define SYNTAQLITE_TK_FOR                            43
#define SYNTAQLITE_TK_IGNORE                         44
#define SYNTAQLITE_TK_IMMEDIATE                      45
#define SYNTAQLITE_TK_INITIALLY                      46
#define SYNTAQLITE_TK_INSTEAD                        47
#define SYNTAQLITE_TK_NO                             48
#define SYNTAQLITE_TK_PLAN                           49
#define SYNTAQLITE_TK_QUERY                          50
#define SYNTAQLITE_TK_KEY                            51
#define SYNTAQLITE_TK_OF                             52
#define SYNTAQLITE_TK_OFFSET                         53
#define SYNTAQLITE_TK_PRAGMA                         54
#define SYNTAQLITE_TK_RAISE                          55
#define SYNTAQLITE_TK_RECURSIVE                      56
#define SYNTAQLITE_TK_RELEASE                        57
#define SYNTAQLITE_TK_REPLACE                        58
#define SYNTAQLITE_TK_RESTRICT                       59
#define SYNTAQLITE_TK_ROW                            60
#define SYNTAQLITE_TK_ROWS                           61
#define SYNTAQLITE_TK_ROLLBACK                       62
#define SYNTAQLITE_TK_SAVEPOINT                      63
#define SYNTAQLITE_TK_TEMP                           64
#define SYNTAQLITE_TK_TRIGGER                        65
#define SYNTAQLITE_TK_VACUUM                         66
#define SYNTAQLITE_TK_VIEW                           67
#define SYNTAQLITE_TK_VIRTUAL                        68
#define SYNTAQLITE_TK_WITH                           69
#define SYNTAQLITE_TK_WITHOUT                        70
#define SYNTAQLITE_TK_NULLS                          71
#define SYNTAQLITE_TK_FIRST                          72
#define SYNTAQLITE_TK_LAST                           73
#define SYNTAQLITE_TK_CURRENT                        74
#define SYNTAQLITE_TK_FOLLOWING                      75
#define SYNTAQLITE_TK_PARTITION                      76
#define SYNTAQLITE_TK_PRECEDING                      77
#define SYNTAQLITE_TK_RANGE                          78
#define SYNTAQLITE_TK_UNBOUNDED                      79
#define SYNTAQLITE_TK_EXCLUDE                        80
#define SYNTAQLITE_TK_GROUPS                         81
#define SYNTAQLITE_TK_OTHERS                         82
#define SYNTAQLITE_TK_TIES                           83
#define SYNTAQLITE_TK_GENERATED                      84
#define SYNTAQLITE_TK_ALWAYS                         85
#define SYNTAQLITE_TK_WITHIN                         86
#define SYNTAQLITE_TK_MATERIALIZED                   87
#define SYNTAQLITE_TK_REINDEX                        88
#define SYNTAQLITE_TK_RENAME                         89
#define SYNTAQLITE_TK_CTIME_KW                       90
#define SYNTAQLITE_TK_IF                             91
#define SYNTAQLITE_TK_ANY                            92
#define SYNTAQLITE_TK_BITAND                         93
#define SYNTAQLITE_TK_BITOR                          94
#define SYNTAQLITE_TK_LSHIFT                         95
#define SYNTAQLITE_TK_RSHIFT                         96
#define SYNTAQLITE_TK_PLUS                           97
#define SYNTAQLITE_TK_MINUS                          98
#define SYNTAQLITE_TK_STAR                           99
#define SYNTAQLITE_TK_SLASH                          100
#define SYNTAQLITE_TK_REM                            101
#define SYNTAQLITE_TK_CONCAT                         102
#define SYNTAQLITE_TK_PTR                            103
#define SYNTAQLITE_TK_COLLATE                        104
#define SYNTAQLITE_TK_BITNOT                         105
#define SYNTAQLITE_TK_ON                             106
#define SYNTAQLITE_TK_INDEXED                        107
#define SYNTAQLITE_TK_STRING                         108
#define SYNTAQLITE_TK_JOIN_KW                        109
#define SYNTAQLITE_TK_INTEGER                        110
#define SYNTAQLITE_TK_FLOAT                          111
#define SYNTAQLITE_TK_SEMI                           112
#define SYNTAQLITE_TK_LP                             113
#define SYNTAQLITE_TK_ORDER                          114
#define SYNTAQLITE_TK_RP                             115
#define SYNTAQLITE_TK_GROUP                          116
#define SYNTAQLITE_TK_AS                             117
#define SYNTAQLITE_TK_COMMA                          118
#define SYNTAQLITE_TK_DOT                            119
#define SYNTAQLITE_TK_UNION                          120
#define SYNTAQLITE_TK_ALL                            121
#define SYNTAQLITE_TK_EXCEPT                         122
#define SYNTAQLITE_TK_INTERSECT                      123
#define SYNTAQLITE_TK_EXISTS                         124
#define SYNTAQLITE_TK_NULL                           125
#define SYNTAQLITE_TK_DISTINCT                       126
#define SYNTAQLITE_TK_FROM                           127
#define SYNTAQLITE_TK_CASE                           128
#define SYNTAQLITE_TK_WHEN                           129
#define SYNTAQLITE_TK_THEN                           130
#define SYNTAQLITE_TK_ELSE                           131
#define SYNTAQLITE_TK_TABLE                          132
#define SYNTAQLITE_TK_CONSTRAINT                     133
#define SYNTAQLITE_TK_DEFAULT                        134
#define SYNTAQLITE_TK_PRIMARY                        135
#define SYNTAQLITE_TK_UNIQUE                         136
#define SYNTAQLITE_TK_CHECK                          137
#define SYNTAQLITE_TK_REFERENCES                     138
#define SYNTAQLITE_TK_AUTOINCR                       139
#define SYNTAQLITE_TK_INSERT                         140
#define SYNTAQLITE_TK_DELETE                         141
#define SYNTAQLITE_TK_UPDATE                         142
#define SYNTAQLITE_TK_SET                            143
#define SYNTAQLITE_TK_DEFERRABLE                     144
#define SYNTAQLITE_TK_FOREIGN                        145
#define SYNTAQLITE_TK_INTO                           146
#define SYNTAQLITE_TK_VALUES                         147
#define SYNTAQLITE_TK_WHERE                          148
#define SYNTAQLITE_TK_RETURNING                      149
#define SYNTAQLITE_TK_NOTHING                        150
#define SYNTAQLITE_TK_BLOB                           151
#define SYNTAQLITE_TK_QNUMBER                        152
#define SYNTAQLITE_TK_VARIABLE                       153
#define SYNTAQLITE_TK_DROP                           154
#define SYNTAQLITE_TK_INDEX                          155
#define SYNTAQLITE_TK_ALTER                          156
#define SYNTAQLITE_TK_TO                             157
#define SYNTAQLITE_TK_ADD                            158
#define SYNTAQLITE_TK_COMMIT                         159
#define SYNTAQLITE_TK_TRANSACTION                    160
#define SYNTAQLITE_TK_SELECT                         161
#define SYNTAQLITE_TK_HAVING                         162
#define SYNTAQLITE_TK_LIMIT                          163
#define SYNTAQLITE_TK_JOIN                           164
#define SYNTAQLITE_TK_USING                          165
#define SYNTAQLITE_TK_CREATE                         166
#define SYNTAQLITE_TK_WINDOW                         167
#define SYNTAQLITE_TK_OVER                           168
#define SYNTAQLITE_TK_FILTER                         169
#define SYNTAQLITE_TK_COLUMN                         170
#define SYNTAQLITE_TK_AGG_FUNCTION                   171
#define SYNTAQLITE_TK_AGG_COLUMN                     172
#define SYNTAQLITE_TK_TRUEFALSE                      173
#define SYNTAQLITE_TK_FUNCTION                       174
#define SYNTAQLITE_TK_UPLUS                          175
#define SYNTAQLITE_TK_UMINUS                         176
#define SYNTAQLITE_TK_TRUTH                          177
#define SYNTAQLITE_TK_REGISTER                       178
#define SYNTAQLITE_TK_VECTOR                         179
#define SYNTAQLITE_TK_SELECT_COLUMN                  180
#define SYNTAQLITE_TK_IF_NULL_ROW                    181
#define SYNTAQLITE_TK_ASTERISK                       182
#define SYNTAQLITE_TK_SPAN                           183
#define SYNTAQLITE_TK_ERROR                          184
#define SYNTAQLITE_TK_SPACE                          185
#define SYNTAQLITE_TK_COMMENT                        186
#define SYNTAQLITE_TK_ILLEGAL                        187
#define SYNTAQLITE_TK_BANG                           188
#define SYNTAQLITE_TK_PERFETTO                       189
#define SYNTAQLITE_TK_MODULE                         190
#define SYNTAQLITE_TK_RETURNS                        191
#define SYNTAQLITE_TK_MACRO                          192
#define SYNTAQLITE_TK_DELEGATES                      193
#define SYNTAQLITE_TK_INCLUDE                        194
#define SYNTAQLITE_TK_TREE                           195
#define SYNTAQLITE_TK_ACCUMULATE                     196
#define SYNTAQLITE_TK_UP                             197
#define SYNTAQLITE_TK_DOWN                           198
#define SYNTAQLITE_TK_INTERVAL                       199
#define SYNTAQLITE_TK_INTERSECTION                   200
#define SYNTAQLITE_TK_PER                            201
#endif
/**************** End token definitions ***************************************/

/* The next sections is a series of control #defines.
** various aspects of the generated parser.
**    YYCODETYPE         is the data type used to store the integer codes
**                       that represent terminal and non-terminal symbols.
**                       "unsigned char" is used if there are fewer than
**                       256 symbols.  Larger types otherwise.
**    YYNOCODE           is a number of type YYCODETYPE that is not used for
**                       any terminal or nonterminal symbol.
**    YYFALLBACK         If defined, this indicates that one or more tokens
**                       (also known as: "terminal symbols") have fall-back
**                       values which should be used if the original symbol
**                       would not parse.  This permits keywords to sometimes
**                       be used as identifiers, for example.
**    YYACTIONTYPE       is the data type used for "action codes" - numbers
**                       that indicate what to do in response to the next
**                       token.
**    SynqPerfettoParseTOKENTYPE     is the data type used for minor type for terminal
**                       symbols.  Background: A "minor type" is a semantic
**                       value associated with a terminal or non-terminal
**                       symbols.  For example, for an "ID" terminal symbol,
**                       the minor type might be the name of the identifier.
**                       Each non-terminal can have a different minor type.
**                       Terminal symbols all have the same minor type, though.
**                       This macros defines the minor type for terminal 
**                       symbols.
**    YYMINORTYPE        is the data type used for all minor types.
**                       This is typically a union of many types, one of
**                       which is SynqPerfettoParseTOKENTYPE.  The entry in the union
**                       for terminal symbols is called "yy0".
**    YYSTACKDEPTH       is the maximum depth of the parser's stack.  If
**                       zero the stack is dynamically sized using realloc()
**    SynqPerfettoParseARG_SDECL     A static variable declaration for the %extra_argument
**    SynqPerfettoParseARG_PDECL     A parameter declaration for the %extra_argument
**    SynqPerfettoParseARG_PARAM     Code to pass %extra_argument as a subroutine parameter
**    SynqPerfettoParseARG_STORE     Code to store %extra_argument into yypParser
**    SynqPerfettoParseARG_FETCH     Code to extract %extra_argument from yypParser
**    SynqPerfettoParseCTX_*         As SynqPerfettoParseARG_ except for %extra_context
**    YYREALLOC          Name of the realloc() function to use
**    YYFREE             Name of the free() function to use
**    YYDYNSTACK         True if stack space should be extended on heap
**    YYERRORSYMBOL      is the code number of the error symbol.  If not
**                       defined, then do no error processing.
**    YYNSTATE           the combined number of states.
**    YYNRULE            the number of rules in the grammar
**    YYNTOKEN           Number of terminal symbols
**    YY_MAX_SHIFT       Maximum value for shift actions
**    YY_MIN_SHIFTREDUCE Minimum value for shift-reduce actions
**    YY_MAX_SHIFTREDUCE Maximum value for shift-reduce actions
**    YY_ERROR_ACTION    The yy_action[] code for syntax error
**    YY_ACCEPT_ACTION   The yy_action[] code for accept
**    YY_NO_ACTION       The yy_action[] code for no-op
**    YY_MIN_REDUCE      Minimum value for reduce actions
**    YY_MAX_REDUCE      Maximum value for reduce actions
**    YY_MIN_DSTRCTR     Minimum symbol value that has a destructor
**    YY_MAX_DSTRCTR     Maximum symbol value that has a destructor
*/
#ifndef INTERFACE
# define INTERFACE 1
#endif
/************* Begin control #defines *****************************************/
#define YYCODETYPE unsigned short int
#define YYNOCODE 367
#define YYACTIONTYPE unsigned short int
#define YYWILDCARD 92
#define SynqPerfettoParseTOKENTYPE SynqParseToken
typedef union {
  int yyinit;
  SynqPerfettoParseTOKENTYPE yy0;
  SynqUpsertValue yy54;
  SynqInsertCmdValue yy90;
  SynqAliasValue yy119;
  SynqJoinOpValue yy138;
  SynqParenExprlistValue yy194;
  SyntaqliteTemporaryQualifier yy234;
  uint32_t yy381;
  SynqWhereRetValue yy423;
  SynqTransOptValue yy482;
  SynqDeferValue yy581;
  SynqWithValue yy583;
  SynqOnUsingValue yy660;
  int yy686;
  SynqConstraintGroups yy699;
  SynqColumnNameValue yy718;
  int yy735;
} YYMINORTYPE;
#ifndef YYSTACKDEPTH
#define YYSTACKDEPTH 100
#endif
#define SynqPerfettoParseARG_SDECL
#define SynqPerfettoParseARG_PDECL
#define SynqPerfettoParseARG_PARAM
#define SynqPerfettoParseARG_FETCH
#define SynqPerfettoParseARG_STORE
#define YYREALLOC synq_stack_realloc
#define YYFREE synq_stack_free
#define YYDYNSTACK 1
#define SynqPerfettoParseCTX_SDECL SynqParseCtx* pCtx;
#define SynqPerfettoParseCTX_PDECL ,SynqParseCtx* pCtx
#define SynqPerfettoParseCTX_PARAM ,pCtx
#define SynqPerfettoParseCTX_FETCH SynqParseCtx* pCtx=yypParser->pCtx;
#define SynqPerfettoParseCTX_STORE yypParser->pCtx=pCtx;
#define YYERRORSYMBOL 206
#define YYERRSYMDT yy735
#define YYFALLBACK 1
#define YYNSTATE             717
#define YYNRULE              481
#define YYNRULE_WITH_ACTION  481
#define YYNTOKEN             202
#define YY_MAX_SHIFT         716
#define YY_MIN_SHIFTREDUCE   1024
#define YY_MAX_SHIFTREDUCE   1504
#define YY_ERROR_ACTION      1505
#define YY_ACCEPT_ACTION     1506
#define YY_NO_ACTION         1507
#define YY_MIN_REDUCE        1508
#define YY_MAX_REDUCE        1988
#define YY_MIN_DSTRCTR       0
#define YY_MAX_DSTRCTR       0
/************* End control #defines *******************************************/
#define YY_NLOOKAHEAD ((int)(sizeof(yy_lookahead)/sizeof(yy_lookahead[0])))

/* Define the yytestcase() macro to be a no-op if is not already defined
** otherwise.
**
** Applications can choose to define yytestcase() in the %include section
** to a macro that can assist in verifying code coverage.  For production
** code the yytestcase() macro should be turned off.  But it is useful
** for testing.
*/
#ifndef yytestcase
# define yytestcase(X)
#endif

/* Macro to determine if stack space has the ability to grow using
** heap memory.
*/
#if YYSTACKDEPTH<=0 || YYDYNSTACK
# define YYGROWABLESTACK 1
#else
# define YYGROWABLESTACK 0
#endif

/* Guarantee a minimum number of initial stack slots.
*/
#if YYSTACKDEPTH<=0
# undef YYSTACKDEPTH
# define YYSTACKDEPTH 2  /* Need a minimum stack size */
#endif


/* Next are the tables used to determine what action to take based on the
** current state and lookahead token.  These tables are used to implement
** functions that take a state number and lookahead value and return an
** action integer.  
**
** Suppose the action integer is N.  Then the action is determined as
** follows
**
**   0 <= N <= YY_MAX_SHIFT             Shift N.  That is, push the lookahead
**                                      token onto the stack and goto state N.
**
**   N between YY_MIN_SHIFTREDUCE       Shift to an arbitrary state then
**     and YY_MAX_SHIFTREDUCE           reduce by rule N-YY_MIN_SHIFTREDUCE.
**
**   N == YY_ERROR_ACTION               A syntax error has occurred.
**
**   N == YY_ACCEPT_ACTION              The parser accepts its input.
**
**   N == YY_NO_ACTION                  No such action.  Denotes unused
**                                      slots in the yy_action[] table.
**
**   N between YY_MIN_REDUCE            Reduce by rule N-YY_MIN_REDUCE
**     and YY_MAX_REDUCE
**
** The action table is constructed as a single large table named yy_action[].
** Given state S and lookahead X, the action is computed as either:
**
**    (A)   N = yy_action[ yy_shift_ofst[S] + X ]
**    (B)   N = yy_default[S]
**
** The (A) formula is preferred.  The B formula is used instead if
** yy_lookahead[yy_shift_ofst[S]+X] is not equal to X.
**
** The formulas above are for computing the action when the lookahead is
** a terminal symbol.  If the lookahead is a non-terminal (as occurs after
** a reduce action) then the yy_reduce_ofst[] array is used in place of
** the yy_shift_ofst[] array.
**
** The following are the tables generated in this section:
**
**  yy_action[]        A single table containing all actions.
**  yy_lookahead[]     A table containing the lookahead for each entry in
**                     yy_action.  Used to detect hash collisions.
**  yy_shift_ofst[]    For each state, the offset into yy_action for
**                     shifting terminals.
**  yy_reduce_ofst[]   For each state, the offset into yy_action for
**                     shifting non-terminals after a reduce.
**  yy_default[]       Default action for each state.
**
*********** Begin parsing tables **********************************************/
#define YY_ACTTAB_COUNT (2578)
static const YYACTIONTYPE yy_action[] = {
 /*     0 */   263, 1579, 1590, 1226, 1573, 1236,   98,  100, 1576,  330,
 /*    10 */  1764,  652,  330, 1764,  336, 1237, 1709, 1839,  330, 1764,
 /*    20 */   470, 1573,   91,   92,  482,   48, 1571, 1070, 1070, 1067,
 /*    30 */  1052, 1061, 1061,   93,   93,   94,   94,   94,   94, 1885,
 /*    40 */   458,  377,  650, 1570, 1809,   91,   92,  482,   48,  495,
 /*    50 */  1070, 1070, 1067, 1052, 1061, 1061,   93,   93,   94,   94,
 /*    60 */    94,   94, 1199,  613,  121,  541,  330, 1764,  461,  595,
 /*    70 */    94,   94,   94,   94,   97,  709,   70,    6,  325, 1169,
 /*    80 */  1664,  259,  630, 1900,  305,  263, 1705,  259,  615, 1738,
 /*    90 */   107,   98,  100,   90,   90,   90,   90,   96,   96,   95,
 /*   100 */    95,   95,   89,   88,  522,  693,  691,  505,  693,  691,
 /*   110 */  1838,  584,  585, 1684,  693,  691,   90,   90,   90,   90,
 /*   120 */    96,   96,   95,   95,   95,   89,   88,  522,   90,   90,
 /*   130 */    90,   90,   96,   96,   95,   95,   95,   89,   88,  522,
 /*   140 */  1858,   69,   91,   92,  482,   48,  338, 1070, 1070, 1067,
 /*   150 */  1052, 1061, 1061,   93,   93,   94,   94,   94,   94,  606,
 /*   160 */   413,  411,  693,  691,   91,   92,  482,   48,  701, 1070,
 /*   170 */  1070, 1067, 1052, 1061, 1061,   93,   93,   94,   94,   94,
 /*   180 */    94, 1852,  363, 1199,  420,  122,  416, 1961,  563, 1292,
 /*   190 */   446, 1655, 1905,   53, 1291, 1425,  709, 1425,   90,   90,
 /*   200 */    90,   90,   96,   96,   95,   95,   95,   89,   88,  522,
 /*   210 */  1656,  456, 1918,   90,   90,   90,   90,   96,   96,   95,
 /*   220 */    95,   95,   89,   88,  522,   96,   96,   95,   95,   95,
 /*   230 */    89,   88,  522, 1169, 1684,   90,   90,   90,   90,   96,
 /*   240 */    96,   95,   95,   95,   89,   88,  522,   91,   92,  482,
 /*   250 */    48,  422, 1070, 1070, 1067, 1052, 1061, 1061,   93,   93,
 /*   260 */    94,   94,   94,   94,   95,   95,   95,   89,   88,  522,
 /*   270 */   522, 1223,   91,   92,  482,   48, 1461, 1070, 1070, 1067,
 /*   280 */  1052, 1061, 1061,   93,   93,   94,   94,   94,   94, 1223,
 /*   290 */  1599,  611, 1626, 1223, 1624,  373, 1223,   91,   92,  482,
 /*   300 */    48, 1226, 1070, 1070, 1067, 1052, 1061, 1061,   93,   93,
 /*   310 */    94,   94,   94,   94, 1709,  411,  599,  398,   90,   90,
 /*   320 */    90,   90,   96,   96,   95,   95,   95,   89,   88,  522,
 /*   330 */  1042,  615, 1955,  561,  251, 1461,  654, 1328, 1223, 1224,
 /*   340 */  1223, 1328,  272,   90,   90,   90,   90,   96,   96,   95,
 /*   350 */    95,   95,   89,   88,  522,  615, 1223, 1224, 1223, 1461,
 /*   360 */  1223, 1224, 1223, 1223, 1224, 1223,   37,   54,   90,   90,
 /*   370 */    90,   90,   96,   96,   95,   95,   95,   89,   88,  522,
 /*   380 */   630,  625,  297,  615,  642,  639,  638,  498,  587,  341,
 /*   390 */    91,   92,  482,   48,  637, 1070, 1070, 1067, 1052, 1061,
 /*   400 */  1061,   93,   93,   94,   94,   94,   94,  330, 1764,  463,
 /*   410 */   264,  671, 1223,  344, 1071, 1071, 1068, 1053,   66,  655,
 /*   420 */    91,   92,  482,   48,  249, 1070, 1070, 1067, 1052, 1061,
 /*   430 */  1061,   93,   93,   94,   94,   94,   94,  330, 1764,  465,
 /*   440 */  1501,  346,  304,   91,   92,  482,   48, 1809, 1070, 1070,
 /*   450 */  1067, 1052, 1061, 1061,   93,   93,   94,   94,   94,   94,
 /*   460 */  1986,   90,   90,   90,   90,   96,   96,   95,   95,   95,
 /*   470 */    89,   88,  522, 1582,  524,  523, 1199,  656,  122, 1223,
 /*   480 */  1224, 1223,  495,  424,  330, 1764,  566, 1360, 1360,  709,
 /*   490 */  1223,   90,   90,   90,   90,   96,   96,   95,   95,   95,
 /*   500 */    89,   88,  522,  693,  691,  330, 1764,  345, 1969, 1402,
 /*   510 */   506, 1420, 1062,  643,   90,   90,   90,   90,   96,   96,
 /*   520 */    95,   95,   95,   89,   88,  522,  713, 1684, 1420,  107,
 /*   530 */  1404, 1420, 1882,  693,  691, 1882, 1104,   91,   92,  482,
 /*   540 */    48,  621, 1070, 1070, 1067, 1052, 1061, 1061,   93,   93,
 /*   550 */    94,   94,   94,   94,   89,   88,  522, 1223, 1224, 1223,
 /*   560 */    91,   92,  482,   48,  673, 1070, 1070, 1067, 1052, 1061,
 /*   570 */  1061,   93,   93,   94,   94,   94,   94,  330, 1764, 1870,
 /*   580 */   693,  691, 1407,   91,   92,  482,   48, 1581, 1070, 1070,
 /*   590 */  1067, 1052, 1061, 1061,   93,   93,   94,   94,   94,   94,
 /*   600 */   617,  693,  691,  457,  583, 1406, 1514,  701,   90,   90,
 /*   610 */    90,   90,   96,   96,   95,   95,   95,   89,   88,  522,
 /*   620 */  1962,  363,  330, 1764, 1762, 1956,  340,  371, 1635, 1616,
 /*   630 */  1096,   90,   90,   90,   90,   96,   96,   95,   95,   95,
 /*   640 */    89,   88,  522,  261, 1326,  330, 1764, 1568,  352,  323,
 /*   650 */   183,  557, 1315,  194,   90,   90,   90,   90,   96,   96,
 /*   660 */    95,   95,   95,   89,   88,  522,  286,  438,  330, 1764,
 /*   670 */   702,  455, 1041,  693,  691, 1634, 1438,   91,   92,  482,
 /*   680 */    48,  270, 1070, 1070, 1067, 1052, 1061, 1061,   93,   93,
 /*   690 */    94,   94,   94,   94,  330, 1764,  705,  662, 1315,  657,
 /*   700 */    91,   92,  482,   48,  260, 1070, 1070, 1067, 1052, 1061,
 /*   710 */  1061,   93,   93,   94,   94,   94,   94, 1223,  693,  691,
 /*   720 */   536,  327, 1764,   91,   92,  482,   48, 1453, 1070, 1070,
 /*   730 */  1067, 1052, 1061, 1061,   93,   93,   94,   94,   94,   94,
 /*   740 */  1041,  693,  691, 1628, 1223,  189,  276, 1223,   90,   90,
 /*   750 */    90,   90,   96,   96,   95,   95,   95,   89,   88,  522,
 /*   760 */   113, 1968, 1656, 1975,  693,  691, 1402,  606,  413,  268,
 /*   770 */  1218,   90,   90,   90,   90,   96,   96,   95,   95,   95,
 /*   780 */    89,   88,  522,  214, 1223, 1224, 1223, 1404,  269, 1883,
 /*   790 */   693,  691, 1883, 1201,   90,   90,   90,   90,   96,   96,
 /*   800 */    95,   95,   95,   89,   88,  522,  266,  708,  663, 1892,
 /*   810 */   621, 1223, 1224, 1223, 1223, 1224, 1223,  693,  691,  551,
 /*   820 */   225, 1839,  450,   51, 1635,  371,  214,   91,   92,  482,
 /*   830 */    48,   79, 1070, 1070, 1067, 1052, 1061, 1061,   93,   93,
 /*   840 */    94,   94,   94,   94, 1223,   33, 1331, 1331,  627,   40,
 /*   850 */    91,   92,  482,   48,  356, 1070, 1070, 1067, 1052, 1061,
 /*   860 */  1061,   93,   93,   94,   94,   94,   94,  455, 1921, 1223,
 /*   870 */   117, 1633, 1794,   91,   92,  482,   48, 1226, 1070, 1070,
 /*   880 */  1067, 1052, 1061, 1061,   93,   93,   94,   94,   94,   94,
 /*   890 */  1709,    6,  451,  328, 1764,  410,  660, 1899,   90,   90,
 /*   900 */    90,   90,   96,   96,   95,   95,   95,   89,   88,  522,
 /*   910 */   534, 1223, 1224, 1223, 1838, 1836, 1834,  226,  371, 1223,
 /*   920 */  1243,   90,   90,   90,   90,   96,   96,   95,   95,   95,
 /*   930 */    89,   88,  522,  508,    6,  115, 1223, 1224, 1223,  672,
 /*   940 */  1899,  621,    5,  701,   90,   90,   90,   90,   96,   96,
 /*   950 */    95,   95,   95,   89,   88,  522,  630,  363,   86,  467,
 /*   960 */  1199,  545,   44, 1737,  704,  500, 1049, 1049,  244,   91,
 /*   970 */    92,  482,   48,  709, 1070, 1070, 1067, 1052, 1061, 1061,
 /*   980 */    93,   93,   94,   94,   94,   94, 1223, 1224, 1223,  693,
 /*   990 */   691,   91,   99,  482,   48, 1530, 1070, 1070, 1067, 1052,
 /*  1000 */  1061, 1061,   93,   93,   94,   94,   94,   94,   92,  482,
 /*  1010 */    48, 1684, 1070, 1070, 1067, 1052, 1061, 1061,   93,   93,
 /*  1020 */    94,   94,   94,   94,  482,   48,  347, 1070, 1070, 1067,
 /*  1030 */  1052, 1061, 1061,   93,   93,   94,   94,   94,   94, 1420,
 /*  1040 */    90,   90,   90,   90,   96,   96,   95,   95,   95,   89,
 /*  1050 */    88,  522, 1967,  335,  675,  107, 1420, 1420, 1615, 1420,
 /*  1060 */   494, 1134,   90,   90,   90,   90,   96,   96,   95,   95,
 /*  1070 */    95,   89,   88,  522, 1420,   14, 1223, 1420,   90,   90,
 /*  1080 */    90,   90,   96,   96,   95,   95,   95,   89,   88,  522,
 /*  1090 */   693,  691,  674,   90,   90,   90,   90,   96,   96,   95,
 /*  1100 */    95,   95,   89,   88,  522, 1508, 1199,  187,  121,  278,
 /*  1110 */   513,  379, 1245,  318,  547,   36,  710,   14,  389,  709,
 /*  1120 */   593,  378,  257,  386,  669,  530,  278, 1099,  379, 1978,
 /*  1130 */   318,  252, 1246,  701,  451, 1199,  701,  121,  378, 1226,
 /*  1140 */   386, 1267,  530, 1223, 1224, 1223, 1269,  363,  709,    3,
 /*  1150 */   363, 1199, 1709,   44, 1388, 1173,  503, 1684,  601,  279,
 /*  1160 */   540,  544,  383,  236,  709, 1226,  605,  385,  176, 1174,
 /*  1170 */   645,  206, 1268, 1244,  106,  517,  279, 1231, 1709,  383,
 /*  1180 */  1509,  716,  715, 1514,  385,  176, 1684,  437,  206,  473,
 /*  1190 */   510,  106,  525,  277, 1099, 1228, 1226, 1227, 1229,  330,
 /*  1200 */  1764, 1762, 1684,  428, 1617,  349,  264,  671,  622, 1709,
 /*  1210 */   277,  436, 1229, 1846, 1847,  478, 1040, 1027,  242,  303,
 /*  1220 */   321,  648,  431,  647,  302,  352, 1382,  183, 1809, 1199,
 /*  1230 */   427,  157,  205,  687, 1027,  445, 1907, 1809, 1229, 1230,
 /*  1240 */   486, 1223,  709,  286,  242, 1867,  330, 1764,  686,  205,
 /*  1250 */  1396, 1223,  701,  459, 1199, 1199,  157,   44,  653,  481,
 /*  1260 */   509,  589,  662,  679,  386, 1905,  363,  709,  709,  701,
 /*  1270 */  1223,  334, 1199,  712,   44, 1760,  481,  348,  589,  499,
 /*  1280 */  1684,  386, 1706,  363, 1040,  709,   83, 1223,  334, 1846,
 /*  1290 */  1847,  507,    2,  623,  535,  693,  691,  536,  634,  533,
 /*  1300 */   626,  301,  528,  604,  560, 1684, 1684,  685, 1223, 1224,
 /*  1310 */  1223,  535, 1199, 1199,  155,  157,  533,  698, 1223, 1224,
 /*  1320 */  1223,  560,  548, 1684,  105,  709,  709,  261,  300,  299,
 /*  1330 */   298,  107,  685,  712,  395,  453, 1315, 1223, 1224, 1223,
 /*  1340 */  1975,  263,  693,  691,  524,  523,   83,   98,  100,  205,
 /*  1350 */   555,    8, 1231, 1356, 1223, 1224, 1223, 1360, 1360,   85,
 /*  1360 */    85, 1809,  528, 1684, 1684, 1317, 1199,   84,  132,  528,
 /*  1370 */   699,  528, 1227, 1229, 1893,    4, 1226,  698,  447,  709,
 /*  1380 */   487, 1358, 1315,  330, 1764,  707,  706, 1229, 1357, 1709,
 /*  1390 */    27,  658,  683, 1261, 1316, 1603,  677, 1199,  499,   44,
 /*  1400 */   339,  676,  233,   94,   94,   94,   94,  456, 1918,  701,
 /*  1410 */   709, 1664, 1231, 1229, 1230, 1232, 1245, 1684,  427,   85,
 /*  1420 */    85,  560, 1231,  363,  514, 1226,  468,   84,    6,  528,
 /*  1430 */   699,  528, 1227, 1229, 1897,    4, 1246,  278, 1709,  379,
 /*  1440 */  1228,  318, 1227, 1229,  688,  185,  706, 1229, 1684,  378,
 /*  1450 */    27,  386,  174,  240, 1635,  242,  654, 1229,  499,  697,
 /*  1460 */  1289,   90,   90,   90,   90,   96,   96,   95,   95,   95,
 /*  1470 */    89,   88,  522, 1229, 1230, 1232, 1199, 1244,   44,  693,
 /*  1480 */   691,  234,    6, 1229, 1230,  263,  562,  279, 1898,  709,
 /*  1490 */   383,   98,  100, 1518,  582,  385,  176,  455, 1635,  206,
 /*  1500 */   119, 1633,  106, 1199, 1759,  157, 1199,  684,  157,  181,
 /*  1510 */   527,  712, 1199,  241,   44, 1635,  709,  682, 1199,  709,
 /*  1520 */   157,  277,  448,  367,   83,  709,  326, 1684, 1289,  263,
 /*  1530 */   297,  709,  642,  639,  638,   98,  100,  186, 1970,  655,
 /*  1540 */   528,  455,  637,    6,  116, 1633,  441,  681, 1954, 1896,
 /*  1550 */   390, 1199,  392,  157, 1684,  698, 1516, 1684,  455, 1960,
 /*  1560 */   205,  118, 1633, 1684,  709, 1199,  619,  157,  440, 1684,
 /*  1570 */  1199, 1602,  157,  680,  677, 1199, 1236,  164,  709,  678,
 /*  1580 */   701,  685, 1431,  709,  685, 1122, 1237,  481,  709,  589,
 /*  1590 */  1231,  712,  386, 1199,  363,  157,  358,   85,   85,  334,
 /*  1600 */  1128, 1226, 1684,  564,   83,   84,  709,  528,  699,  528,
 /*  1610 */  1227, 1229,  501,    4, 1709, 1431, 1684,  393,  456, 1918,
 /*  1620 */   528, 1684,  535,  646,  706, 1229, 1684,  533,   27,  519,
 /*  1630 */     6, 1129,  560, 1226, 1123,  698, 1895, 1226, 1746,  488,
 /*  1640 */   391,  711,  488,  520, 1684, 1420, 1709,  301,  521,  620,
 /*  1650 */  1709, 1229, 1230, 1232,  677, 1199, 1226,   44, 1285,  676,
 /*  1660 */   526, 1923, 1420,  712,  607, 1420, 1226,  252,  709, 1709,
 /*  1670 */  1231,  359,  408, 1199,  591,  168,   83,   85,   85, 1709,
 /*  1680 */  1761,  456, 1918,   17, 1462,   84,  709,  528,  699,  528,
 /*  1690 */  1227, 1229,  528,    4,  553,  554, 1199, 1199,   46,  137,
 /*  1700 */   712, 1958,   74,  609,  706, 1229, 1684,  698,   27,  664,
 /*  1710 */   709,  342,  630,   47, 1226, 1199, 1741,  138,  511, 1736,
 /*  1720 */  1107,  477,  476, 1199, 1684,  139,  487, 1709,  709,  528,
 /*  1730 */   376, 1229, 1230, 1232,  257, 1740,  709,   65, 1199, 1199,
 /*  1740 */   140,   45, 1231, 1462,  698,  588,  380, 1684, 1684,   85,
 /*  1750 */    85,  709,  709,  597,  479, 1468, 1469,   84,  381,  528,
 /*  1760 */   699,  528, 1227, 1229,   67,    4, 1684, 1462, 1226, 1226,
 /*  1770 */  1226, 1199,  712,  123, 1684,  103,  706, 1229,  695, 1231,
 /*  1780 */    27, 1709, 1709, 1709,  709,   83,   85,   85, 1107, 1684,
 /*  1790 */  1684,  489,  215, 1739,   84,  382,  528,  699,  528, 1227,
 /*  1800 */  1229,  528,    4, 1229, 1230, 1232, 1407, 1756,  712,  598,
 /*  1810 */   479,  608,  479,  706, 1229,  694,  698,   27,  612,  479,
 /*  1820 */  1988,   83, 1684,  357,  479,   56,  248,  457,  665, 1403,
 /*  1830 */   590, 1199, 1199,  124,  141, 1498,   74,  528,  546,  462,
 /*  1840 */  1229, 1230, 1232, 1289,  709,  709,  689, 1732, 1772,  630,
 /*  1850 */  1292, 1231,  698, 1120,  102, 1291, 1735, 1113,   85,   85,
 /*  1860 */  1284, 1662, 1121,   87,   66,   81,   84,  329,  528,  699,
 /*  1870 */   528, 1227, 1229, 1707,    4, 1199, 1661,  142, 1199, 1160,
 /*  1880 */   143, 1454, 1684, 1684,  571,  706, 1229, 1231,  709,   27,
 /*  1890 */   369,  709,  596,  204,   85,   85, 1506,    1, 1510,  716,
 /*  1900 */   715, 1514,   84,  594,  528,  699,  528, 1227, 1229, 1153,
 /*  1910 */     4, 1289, 1229, 1230, 1232,  550,   10,  330, 1764, 1762,
 /*  1920 */   186,  706, 1229,  656,  683,   27, 1684, 1233,   76, 1684,
 /*  1930 */   275, 1199,  274,  144, 1450, 1239, 1240,  571, 1199, 1199,
 /*  1940 */   145,  125, 1881,  352,  709,  183, 1226, 1160, 1229, 1230,
 /*  1950 */  1232,  709,  709, 1601, 1199, 1199,  126,  127, 1199, 1709,
 /*  1960 */   128,  286,  283,  403,  285,  435,   74,  709,  709,  401,
 /*  1970 */  1199,  709,  146, 1199,  602,  147, 1361, 1361, 1199, 1199,
 /*  1980 */   148,  149, 1684,  709,  434, 1199,  709,  150,  404, 1684,
 /*  1990 */  1684,  709,  709, 1827, 1199, 1233,  120,  409,  709, 1190,
 /*  2000 */    74,  614,  307, 1825,  307, 1684, 1684,  709,  415, 1684,
 /*  2010 */     2,  714,  190,  693,  691,  536,  616, 1359, 1359,  307,
 /*  2020 */  1199, 1684,  129,  635, 1684,  630,  311,  419,  425, 1684,
 /*  2030 */  1684,   74,  418,  709,  421,  423, 1684, 1620, 1199, 1199,
 /*  2040 */   130,   43, 1600,  430, 1199, 1684,  131, 1199, 1199,  151,
 /*  2050 */   162,  709,  709, 1320, 1578, 1572,  307,  709, 1975, 1156,
 /*  2060 */   709,  709,  311, 1792,  231, 1199, 1199,  163,  152, 1806,
 /*  2070 */  1808, 1684, 1912, 1199, 1542,  133,  452, 1529,  709,  709,
 /*  2080 */   696, 1199, 1392,  153, 1391,   76,  709,   76, 1390, 1684,
 /*  2090 */  1684,   76, 1951, 1038,  709, 1684,  188,  387, 1684, 1684,
 /*  2100 */   531, 1199, 1199,  134,  159, 1199, 1199,  199,  200, 1199,
 /*  2110 */   195,  154,  631,   74,  709,  709, 1684, 1684,  709,  709,
 /*  2120 */  1950, 1199,  709,  135, 1684, 1199, 1199,  197,  198, 1199,
 /*  2130 */   319,  170, 1684, 1199,  709,  158,  254,  353,  709,  709,
 /*  2140 */   354,  355,  709, 1767,  196, 1199,  709,  160, 1199,   12,
 /*  2150 */   165,  492, 1684, 1684, 1966,    9, 1684, 1684,  709,  581,
 /*  2160 */  1684,  709, 1199,  180,  169, 1199, 1650,  191, 1199,  397,
 /*  2170 */   171,  337, 1684,  400,  406,  709, 1684, 1684,  709,  618,
 /*  2180 */  1684,  709, 1678, 1677, 1684, 1199,  640,  166, 1199, 1199,
 /*  2190 */   161,  167, 1199, 1199,  156,  136, 1684,  407,  709, 1684,
 /*  2200 */   343,  709,  709,  471,  412,  709,  709,  258,  433, 1599,
 /*  2210 */   444, 1565,  237, 1684,  253, 1797, 1684, 1798,  238, 1684,
 /*  2220 */  1796, 1795, 1641, 1642,  700, 1500, 1455,  490,  374,  491,
 /*  2230 */   493,  565,  556,  333,  217,  310, 1684,  466,  317, 1684,
 /*  2240 */  1684,  579,  580, 1684, 1684,  570,  574,   51, 1859, 1370,
 /*  2250 */  1851,  496,   52, 1849,  497,  567,   55, 1273,  175,  280,
 /*  2260 */   177,  586, 1261,  592, 1748, 1747,  250, 1651,  108,  375,
 /*  2270 */   223,  178,  109,   71, 1649,  110,  111,  112,   32,  683,
 /*  2280 */   211,  396,  220, 1648,  399,  600,  633,  288,   67,  603,
 /*  2290 */   290, 1865, 1680,  469, 1679,  610,   38,  472, 1652,  227,
 /*  2300 */   235,  624,  414,  293,   61, 1811,  629,  417,  320,  294,
 /*  2310 */  1566,  295,  474,  649, 1623, 1622, 1621,  512,   63, 1610,
 /*  2320 */   475, 1587, 1113, 1593, 1592, 1586,  432, 1609,   13, 1585,
 /*  2330 */   308, 1766, 1584,  651,  439,   68, 1765,  243,  659,  442,
 /*  2340 */   443,  309,   11, 1903,  515, 1540, 1902,  449, 1718,  516,
 /*  2350 */  1719,  362,   80,  454,  518,  360,  264,  361,  703, 1831,
 /*  2360 */  1917,  484,  485,  216,  201,  537, 1832, 1830, 1829,  184,
 /*  2370 */   202, 1980,  350,   34,  255, 1979,   35,  256, 1977,   49,
 /*  2380 */   203, 1380,  315,  529,  483,  368,  532,   50,  265,  247,
 /*  2390 */   460,  364,  539,  538, 1459, 1460,  542,  543,  370,  245,
 /*  2400 */  1456,  365,  267, 1497,  549,  366,  331,  571,  552,  271,
 /*  2410 */   246,  464,  172,  558,  559,  332,   15, 1465,  218,  273,
 /*  2420 */   316,  239, 1448,  372,  575,  568,  569, 1447,  572,  573,
 /*  2430 */  1442,  576,  577,  578, 1452,  480, 1440, 1378, 1353, 1351,
 /*  2440 */   384,  281,  388,   31,  179,  282, 1249,  284,  394, 1285,
 /*  2450 */   287,  402,   16,  221,  289, 1339,  222,  207,  502,  219,
 /*  2460 */   208,  209,  504,   57,   58,   59,   60,  224,  291, 1344,
 /*  2470 */   210,  405,  192, 1329,  228,   39,  292, 1338, 1335,  628,
 /*  2480 */   114,  632,  307, 1385,  434,  182,  229,  296,  636,   62,
 /*  2490 */    18,  641, 1111,   19,  426,  644, 1124,  351,  429,   64,
 /*  2500 */   212,  306,  173,  213,  322,  324, 1323,   20,  230, 1318,
 /*  2510 */   262,   76, 1410,  104,  232,  661,  193,   72,  666,  667,
 /*  2520 */    73,  668,  670, 1436,   21,   22,   23, 1426, 1422, 1424,
 /*  2530 */     7, 1430,   74,   24, 1060, 1429, 1154, 1074, 1055, 1054,
 /*  2540 */    25,   26,   77,   75,  690,   29,   30,   78, 1507,  312,
 /*  2550 */  1242,  692,   82, 1507,   28,   41, 1507, 1708, 1148, 1051,
 /*  2560 */  1048,   42, 1050, 1507,  313, 1507, 1507, 1507, 1507, 1507,
 /*  2570 */  1507, 1039, 1029, 1035,  314, 1507,  101, 1028,
};
static const YYCODETYPE yy_lookahead[] = {
 /*     0 */   221,  257,  237,  206,  219,    5,  227,  228,  243,  223,
 /*    10 */   224,  225,  223,  224,  225,   15,  219,  219,  223,  224,
 /*    20 */   225,  219,   22,   23,   24,   25,  241,   27,   28,   29,
 /*    30 */    30,   31,   32,   33,   34,   35,   36,   37,   38,  324,
 /*    40 */   325,  219,  240,  241,  219,   22,   23,   24,   25,  219,
 /*    50 */    27,   28,   29,   30,   31,   32,   33,   34,   35,   36,
 /*    60 */    37,   38,  206,  268,  208,   40,  223,  224,  225,  270,
 /*    70 */    35,   36,   37,   38,   39,  219,   53,  327,  298,   58,
 /*    80 */   281,  301,  285,  333,  298,  221,  212,  301,  219,  292,
 /*    90 */    69,  227,  228,   93,   94,   95,   96,   97,   98,   99,
 /*   100 */   100,  101,  102,  103,  104,  319,  320,  282,  319,  320,
 /*   110 */   312,  313,  314,  257,  319,  320,   93,   94,   95,   96,
 /*   120 */    97,   98,   99,  100,  101,  102,  103,  104,   93,   94,
 /*   130 */    95,   96,   97,   98,   99,  100,  101,  102,  103,  104,
 /*   140 */   318,  118,   22,   23,   24,   25,  277,   27,   28,   29,
 /*   150 */    30,   31,   32,   33,   34,   35,   36,   37,   38,  141,
 /*   160 */   142,  140,  319,  320,   22,   23,   24,   25,  147,   27,
 /*   170 */    28,   29,   30,   31,   32,   33,   34,   35,   36,   37,
 /*   180 */    38,  317,  161,  206,   65,  208,   67,  357,  358,  121,
 /*   190 */   334,  261,  336,   51,  126,   75,  219,   77,   93,   94,
 /*   200 */    95,   96,   97,   98,   99,  100,  101,  102,  103,  104,
 /*   210 */   280,  337,  338,   93,   94,   95,   96,   97,   98,   99,
 /*   220 */   100,  101,  102,  103,  104,   97,   98,   99,  100,  101,
 /*   230 */   102,  103,  104,   58,  257,   93,   94,   95,   96,   97,
 /*   240 */    98,   99,  100,  101,  102,  103,  104,   22,   23,   24,
 /*   250 */    25,  132,   27,   28,   29,   30,   31,   32,   33,   34,
 /*   260 */    35,   36,   37,   38,   99,  100,  101,  102,  103,  104,
 /*   270 */   104,   40,   22,   23,   24,   25,   40,   27,   28,   29,
 /*   280 */    30,   31,   32,   33,   34,   35,   36,   37,   38,   40,
 /*   290 */   235,  106,  237,   40,  239,   40,   40,   22,   23,   24,
 /*   300 */    25,  206,   27,   28,   29,   30,   31,   32,   33,   34,
 /*   310 */    35,   36,   37,   38,  219,  140,  141,  142,   93,   94,
 /*   320 */    95,   96,   97,   98,   99,  100,  101,  102,  103,  104,
 /*   330 */    99,  219,  355,  356,  149,   99,   24,    3,  107,  108,
 /*   340 */   109,    7,  117,   93,   94,   95,   96,   97,   98,   99,
 /*   350 */   100,  101,  102,  103,  104,  219,  107,  108,  109,  123,
 /*   360 */   107,  108,  109,  107,  108,  109,  113,  117,   93,   94,
 /*   370 */    95,   96,   97,   98,   99,  100,  101,  102,  103,  104,
 /*   380 */   285,   47,  133,  219,  135,  136,  137,  292,  293,  277,
 /*   390 */    22,   23,   24,   25,  145,   27,   28,   29,   30,   31,
 /*   400 */    32,   33,   34,   35,   36,   37,   38,  223,  224,  225,
 /*   410 */   168,  169,   40,  277,   27,   28,   29,   30,  106,  107,
 /*   420 */    22,   23,   24,   25,  149,   27,   28,   29,   30,   31,
 /*   430 */    32,   33,   34,   35,   36,   37,   38,  223,  224,  225,
 /*   440 */    92,  277,   70,   22,   23,   24,   25,  219,   27,   28,
 /*   450 */    29,   30,   31,   32,   33,   34,   35,   36,   37,   38,
 /*   460 */   112,   93,   94,   95,   96,   97,   98,   99,  100,  101,
 /*   470 */   102,  103,  104,  257,   97,   98,  206,  165,  208,  107,
 /*   480 */   108,  109,  219,  115,  223,  224,  225,  110,  111,  219,
 /*   490 */    40,   93,   94,   95,   96,   97,   98,   99,  100,  101,
 /*   500 */   102,  103,  104,  319,  320,  223,  224,  225,  219,   92,
 /*   510 */   282,   61,  125,  115,   93,   94,   95,   96,   97,   98,
 /*   520 */    99,  100,  101,  102,  103,  104,   76,  257,   78,   69,
 /*   530 */   113,   81,  115,  319,  320,  118,  115,   22,   23,   24,
 /*   540 */    25,  219,   27,   28,   29,   30,   31,   32,   33,   34,
 /*   550 */    35,   36,   37,   38,  102,  103,  104,  107,  108,  109,
 /*   560 */    22,   23,   24,   25,  114,   27,   28,   29,   30,   31,
 /*   570 */    32,   33,   34,   35,   36,   37,   38,  223,  224,  225,
 /*   580 */   319,  320,   92,   22,   23,   24,   25,  257,   27,   28,
 /*   590 */    29,   30,   31,   32,   33,   34,   35,   36,   37,   38,
 /*   600 */   219,  319,  320,  113,  205,  115,  207,  147,   93,   94,
 /*   610 */    95,   96,   97,   98,   99,  100,  101,  102,  103,  104,
 /*   620 */   357,  161,  223,  224,  225,  355,  304,  219,  219,  245,
 /*   630 */   115,   93,   94,   95,   96,   97,   98,   99,  100,  101,
 /*   640 */   102,  103,  104,  109,   18,  223,  224,  225,  249,  115,
 /*   650 */   251,  362,  118,  115,   93,   94,   95,   96,   97,   98,
 /*   660 */    99,  100,  101,  102,  103,  104,  267,  219,  223,  224,
 /*   670 */   225,  262,   40,  319,  320,  266,  115,   22,   23,   24,
 /*   680 */    25,   67,   27,   28,   29,   30,   31,   32,   33,   34,
 /*   690 */    35,   36,   37,   38,  223,  224,  225,  219,  164,  219,
 /*   700 */    22,   23,   24,   25,  219,   27,   28,   29,   30,   31,
 /*   710 */    32,   33,   34,   35,   36,   37,   38,   40,  319,  320,
 /*   720 */   321,  223,  224,   22,   23,   24,   25,   40,   27,   28,
 /*   730 */    29,   30,   31,   32,   33,   34,   35,   36,   37,   38,
 /*   740 */   108,  319,  320,  261,   40,  113,  132,   40,   93,   94,
 /*   750 */    95,   96,   97,   98,   99,  100,  101,  102,  103,  104,
 /*   760 */    56,  353,  280,  364,  319,  320,   92,  141,  142,  155,
 /*   770 */   115,   93,   94,   95,   96,   97,   98,   99,  100,  101,
 /*   780 */   102,  103,  104,  219,  107,  108,  109,  113,  174,  115,
 /*   790 */   319,  320,  118,  115,   93,   94,   95,   96,   97,   98,
 /*   800 */    99,  100,  101,  102,  103,  104,  192,  219,  330,  331,
 /*   810 */   219,  107,  108,  109,  107,  108,  109,  319,  320,  132,
 /*   820 */   113,  219,  219,  146,  219,  219,  219,   22,   23,   24,
 /*   830 */    25,  130,   27,   28,   29,   30,   31,   32,   33,   34,
 /*   840 */    35,   36,   37,   38,   40,  113,  140,  141,  142,  117,
 /*   850 */    22,   23,   24,   25,  281,   27,   28,   29,   30,   31,
 /*   860 */    32,   33,   34,   35,   36,   37,   38,  262,  219,   40,
 /*   870 */   265,  266,  299,   22,   23,   24,   25,  206,   27,   28,
 /*   880 */    29,   30,   31,   32,   33,   34,   35,   36,   37,   38,
 /*   890 */   219,  327,  219,  223,  224,  304,  332,  333,   93,   94,
 /*   900 */    95,   96,   97,   98,   99,  100,  101,  102,  103,  104,
 /*   910 */   219,  107,  108,  109,  312,  313,  314,  113,  219,   40,
 /*   920 */   115,   93,   94,   95,   96,   97,   98,   99,  100,  101,
 /*   930 */   102,  103,  104,  260,  327,   56,  107,  108,  109,  332,
 /*   940 */   333,  219,  113,  147,   93,   94,   95,   96,   97,   98,
 /*   950 */    99,  100,  101,  102,  103,  104,  285,  161,  130,  353,
 /*   960 */   206,  219,  208,  292,  120,  211,  122,  123,  117,   22,
 /*   970 */    23,   24,   25,  219,   27,   28,   29,   30,   31,   32,
 /*   980 */    33,   34,   35,   36,   37,   38,  107,  108,  109,  319,
 /*   990 */   320,   22,   23,   24,   25,  224,   27,   28,   29,   30,
 /*  1000 */    31,   32,   33,   34,   35,   36,   37,   38,   23,   24,
 /*  1010 */    25,  257,   27,   28,   29,   30,   31,   32,   33,   34,
 /*  1020 */    35,   36,   37,   38,   24,   25,  304,   27,   28,   29,
 /*  1030 */    30,   31,   32,   33,   34,   35,   36,   37,   38,   61,
 /*  1040 */    93,   94,   95,   96,   97,   98,   99,  100,  101,  102,
 /*  1050 */   103,  104,  353,   89,   76,   69,   78,   61,  115,   81,
 /*  1060 */   361,  118,   93,   94,   95,   96,   97,   98,   99,  100,
 /*  1070 */   101,  102,  103,  104,   78,  218,   40,   81,   93,   94,
 /*  1080 */    95,   96,   97,   98,   99,  100,  101,  102,  103,  104,
 /*  1090 */   319,  320,  114,   93,   94,   95,   96,   97,   98,   99,
 /*  1100 */   100,  101,  102,  103,  104,    0,  206,  118,  208,    4,
 /*  1110 */    24,    6,    1,    8,  219,  350,  215,  218,  154,  219,
 /*  1120 */   134,   16,  118,   18,   86,   20,    4,   40,    6,  364,
 /*  1130 */     8,  127,   21,  147,  219,  206,  147,  208,   16,  206,
 /*  1140 */    18,   14,   20,  107,  108,  109,   19,  161,  219,  113,
 /*  1150 */   161,  206,  219,  208,   64,   44,  211,  257,   68,   54,
 /*  1160 */   347,  348,   57,  306,  219,  206,  309,   62,   63,   58,
 /*  1170 */    84,   66,   45,   62,   69,  260,   54,   90,  219,   57,
 /*  1180 */   204,  205,  206,  207,   62,   63,  257,  254,   66,  256,
 /*  1190 */   104,   69,  215,   88,  107,  108,  206,  110,  111,  223,
 /*  1200 */   224,  225,  257,  117,  245,  246,  168,  169,  309,  219,
 /*  1210 */    88,  125,  125,  312,  313,  256,   40,  112,  285,  133,
 /*  1220 */   134,  135,  136,  137,  138,  249,  136,  251,  219,  206,
 /*  1230 */   144,  208,  127,  210,  112,  335,  336,  219,  151,  152,
 /*  1240 */   252,   40,  219,  267,  285,  155,  223,  224,  225,  127,
 /*  1250 */   115,   40,  147,  118,  206,  206,  208,  208,  210,  154,
 /*  1260 */   211,  156,  219,  334,  159,  336,  161,  219,  219,  147,
 /*  1270 */    40,  166,  206,   11,  208,  285,  154,  211,  156,  219,
 /*  1280 */   257,  159,  212,  161,  108,  219,   24,   40,  166,  312,
 /*  1290 */   313,  282,  316,  305,  189,  319,  320,  321,   24,  194,
 /*  1300 */   282,   27,   40,  315,  199,  257,  257,  284,  107,  108,
 /*  1310 */   109,  189,  206,  206,  208,  208,  194,   55,  107,  108,
 /*  1320 */   109,  199,  219,  257,  113,  219,  219,  109,  140,  141,
 /*  1330 */   142,   69,  284,   11,  274,  229,  118,  107,  108,  109,
 /*  1340 */   364,  221,  319,  320,   97,   98,   24,  227,  228,  127,
 /*  1350 */   219,   29,   90,  106,  107,  108,  109,  110,  111,   97,
 /*  1360 */    98,  219,   40,  257,  257,  164,  206,  105,  208,  107,
 /*  1370 */   108,  109,  110,  111,  331,  113,  206,   55,  258,  219,
 /*  1380 */   106,  134,  164,  223,  224,  225,  124,  125,  141,  219,
 /*  1390 */   128,  284,  114,   41,  164,  236,   74,  206,  219,  208,
 /*  1400 */   270,   79,  211,   35,   36,   37,   38,  337,  338,  147,
 /*  1410 */   219,  281,   90,  151,  152,  153,    1,  257,  144,   97,
 /*  1420 */    98,  199,   90,  161,  282,  206,  256,  105,  327,  107,
 /*  1430 */   108,  109,  110,  111,  333,  113,   21,    4,  219,    6,
 /*  1440 */   108,    8,  110,  111,  284,  167,  124,  125,  257,   16,
 /*  1450 */   128,   18,  161,  274,  219,  285,   24,  125,  219,   44,
 /*  1460 */    40,   93,   94,   95,   96,   97,   98,   99,  100,  101,
 /*  1470 */   102,  103,  104,  151,  152,  153,  206,   62,  208,  319,
 /*  1480 */   320,  211,  327,  151,  152,  221,  195,   54,  333,  219,
 /*  1490 */    57,  227,  228,  212,   22,   62,   63,  262,  219,   66,
 /*  1500 */   265,  266,   69,  206,  285,  208,  206,  210,  208,  157,
 /*  1510 */   210,   11,  206,  274,  208,  219,  219,  211,  206,  219,
 /*  1520 */   208,   88,  258,  219,   24,  219,  218,  257,  108,  221,
 /*  1530 */   133,  219,  135,  136,  137,  227,  228,  117,  219,  107,
 /*  1540 */    40,  262,  145,  327,  265,  266,  275,   48,  219,  333,
 /*  1550 */    65,  206,   67,  208,  257,   55,  212,  257,  262,  219,
 /*  1560 */   127,  265,  266,  257,  219,  206,   24,  208,  297,  257,
 /*  1570 */   206,  236,  208,   74,   74,  206,    5,  208,  219,   79,
 /*  1580 */   147,  284,   83,  219,  284,   10,   15,  154,  219,  156,
 /*  1590 */    90,   11,  159,  206,  161,  208,  284,   97,   98,  166,
 /*  1600 */    14,  206,  257,  219,   24,  105,  219,  107,  108,  109,
 /*  1610 */   110,  111,   42,  113,  219,  116,  257,  132,  337,  338,
 /*  1620 */    40,  257,  189,   48,  124,  125,  257,  194,  128,  284,
 /*  1630 */   327,   45,  199,  206,   59,   55,  333,  206,  213,  214,
 /*  1640 */   155,  213,  214,  284,  257,   61,  219,   27,  284,  107,
 /*  1650 */   219,  151,  152,  153,   74,  206,  206,  208,  118,   79,
 /*  1660 */   211,  189,   78,   11,  142,   81,  206,  127,  219,  219,
 /*  1670 */    90,  284,  150,  206,  189,  208,   24,   97,   98,  219,
 /*  1680 */   285,  337,  338,  113,   40,  105,  219,  107,  108,  109,
 /*  1690 */   110,  111,   40,  113,  341,  342,  206,  206,  208,  208,
 /*  1700 */    11,  219,  118,  142,  124,  125,  257,   55,  128,  219,
 /*  1710 */   219,  150,  285,   24,  206,  206,  285,  208,  143,  292,
 /*  1720 */    40,   97,   98,  206,  257,  208,  106,  219,  219,   40,
 /*  1730 */   219,  151,  152,  153,  118,  285,  219,  113,  206,  206,
 /*  1740 */   208,  208,   90,   99,   55,  285,  219,  257,  257,   97,
 /*  1750 */    98,  219,  219,  216,  217,  197,  198,  105,  219,  107,
 /*  1760 */   108,  109,  110,  111,  148,  113,  257,  123,  206,  206,
 /*  1770 */   206,  206,   11,  208,  257,   34,  124,  125,  126,   90,
 /*  1780 */   128,  219,  219,  219,  219,   24,   97,   98,  108,  257,
 /*  1790 */   257,  322,  323,  285,  105,  219,  107,  108,  109,  110,
 /*  1800 */   111,   40,  113,  151,  152,  153,   92,  219,   11,  216,
 /*  1810 */   217,  216,  217,  124,  125,  126,   55,  128,  216,  217,
 /*  1820 */   219,   24,  257,  216,  217,  148,  149,  113,   99,  115,
 /*  1830 */   219,  206,  206,  208,  208,  115,  118,   40,  118,  117,
 /*  1840 */   151,  152,  153,   40,  219,  219,   24,  285,  285,  285,
 /*  1850 */   121,   90,   55,  125,  113,  126,  292,  139,   97,   98,
 /*  1860 */    99,  219,  134,  129,  106,  131,  105,  113,  107,  108,
 /*  1870 */   109,  110,  111,  119,  113,  206,  219,  208,  206,   40,
 /*  1880 */   208,  115,  257,  257,  118,  124,  125,   90,  219,  128,
 /*  1890 */   115,  219,  219,  118,   97,   98,  202,  203,  204,  205,
 /*  1900 */   206,  207,  105,  268,  107,  108,  109,  110,  111,   87,
 /*  1910 */   113,  108,  151,  152,  153,  193,  113,  223,  224,  225,
 /*  1920 */   117,  124,  125,  165,  114,  128,  257,   40,  118,  257,
 /*  1930 */   117,  206,  119,  208,  115,   72,   73,  118,  206,  206,
 /*  1940 */   208,  208,  219,  249,  219,  251,  206,  108,  151,  152,
 /*  1950 */   153,  219,  219,  236,  206,  206,  208,  208,  206,  219,
 /*  1960 */   208,  267,  117,  115,  119,  125,  118,  219,  219,  219,
 /*  1970 */   206,  219,  208,  206,  219,  208,  110,  111,  206,  206,
 /*  1980 */   208,  208,  257,  219,  144,  206,  219,  208,  219,  257,
 /*  1990 */   257,  219,  219,  219,  206,  108,  208,  115,  219,  115,
 /*  2000 */   118,  115,  118,  219,  118,  257,  257,  219,  219,  257,
 /*  2010 */   316,  114,  115,  319,  320,  321,  115,  110,  111,  118,
 /*  2020 */   206,  257,  208,  115,  257,  285,  118,  219,  115,  257,
 /*  2030 */   257,  118,  292,  219,  219,  219,  257,  219,  206,  206,
 /*  2040 */   208,  208,  219,  219,  206,  257,  208,  206,  206,  208,
 /*  2050 */   208,  219,  219,  115,  219,  219,  118,  219,  364,  115,
 /*  2060 */   219,  219,  118,  300,  294,  206,  206,  208,  208,  219,
 /*  2070 */   219,  257,  329,  206,  219,  208,  219,  219,  219,  219,
 /*  2080 */   280,  206,  115,  208,  115,  118,  219,  118,  115,  257,
 /*  2090 */   257,  118,  294,  115,  219,  257,  118,  288,  257,  257,
 /*  2100 */   349,  206,  206,  208,  208,  206,  206,  208,  208,  206,
 /*  2110 */   115,  208,  307,  118,  219,  219,  257,  257,  219,  219,
 /*  2120 */   294,  206,  219,  208,  257,  206,  206,  208,  208,  206,
 /*  2130 */   302,  208,  257,  206,  219,  208,  232,  294,  219,  219,
 /*  2140 */   294,  294,  219,  294,  220,  206,  219,  208,  206,  209,
 /*  2150 */   208,  345,  257,  257,  359,  354,  257,  257,  219,  339,
 /*  2160 */   257,  219,  206,  291,  208,  206,  272,  208,  206,  271,
 /*  2170 */   208,  278,  257,  271,  310,  219,  257,  257,  219,  310,
 /*  2180 */   257,  219,  272,  272,  257,  206,  233,  208,  206,  206,
 /*  2190 */   208,  208,  206,  206,  208,  208,  257,  283,  219,  257,
 /*  2200 */   283,  219,  219,  272,  278,  219,  219,  247,  258,  235,
 /*  2210 */   278,  250,  275,  257,  209,  299,  257,  299,  275,  257,
 /*  2220 */   299,  299,  258,  258,  231,   92,   40,  366,  113,  346,
 /*  2230 */   342,   94,  343,  352,  201,  119,  257,  363,  222,  257,
 /*  2240 */   257,   40,  165,  257,  257,  340,  340,  146,  318,   13,
 /*  2250 */   222,  342,  311,  222,  222,  343,  311,   63,  290,  160,
 /*  2260 */   290,  289,   41,   91,  289,  289,  149,  273,  291,  344,
 /*  2270 */   113,  291,  287,  163,  276,  287,  287,  287,  279,  114,
 /*  2280 */    22,  275,  269,  276,  275,  222,   91,  253,  148,  222,
 /*  2290 */   253,  283,  273,  283,  273,  283,  279,  283,  269,  269,
 /*  2300 */   113,  259,  222,  253,  129,  308,   43,  303,  222,  253,
 /*  2310 */   222,  253,  259,  106,  242,  242,  242,   46,  113,  238,
 /*  2320 */   259,  242,  139,  248,  248,  233,  242,  238,  118,  242,
 /*  2330 */   222,  276,  242,  255,  275,  162,  276,  296,  116,  295,
 /*  2340 */   283,   80,  113,  328,   71,  226,  328,  222,  286,  104,
 /*  2350 */   286,  230,  129,  259,  117,  264,  168,  264,  263,  218,
 /*  2360 */   338,  326,  326,  323,  244,  365,  218,  218,  218,  234,
 /*  2370 */   244,  351,  234,  350,  232,  351,  350,  232,  351,  218,
 /*  2380 */   244,   49,  190,   50,  119,  360,  189,   34,   54,  113,
 /*  2390 */    40,  117,  115,  191,   40,   40,   40,  118,  360,  113,
 /*  2400 */    40,  113,  106,   40,  157,  113,  191,  118,  115,  118,
 /*  2410 */   113,  117,  113,   52,  200,  196,  118,   35,  118,  117,
 /*  2420 */   115,  117,  119,   40,   40,  119,  119,  119,  119,  119,
 /*  2430 */   115,  119,   40,  113,   40,  189,   58,  112,  115,  115,
 /*  2440 */   157,  106,  158,  132,  157,  155,  124,  117,  147,  118,
 /*  2450 */   165,  132,  113,  127,  106,  112,  127,  143,   42,  146,
 /*  2460 */   143,  143,   12,   34,   34,   34,   34,  146,    9,  107,
 /*  2470 */   143,  155,    8,   52,   52,  117,  119,  112,   60,   17,
 /*  2480 */   106,   24,  118,  124,  144,  119,  113,  138,   51,  113,
 /*  2490 */   113,   51,   40,  113,  115,   85,    2,   51,  117,  113,
 /*  2500 */    12,    9,  115,  118,  115,  115,  107,    9,  113,  164,
 /*  2510 */   119,  118,  115,  113,  118,  117,  115,    9,  114,  113,
 /*  2520 */   148,  116,  113,  115,    9,    9,    9,   60,   77,   75,
 /*  2530 */    23,   60,  118,    9,  115,   82,   87,   18,  115,  115,
 /*  2540 */   113,  113,  127,  118,  118,    9,    9,  127,  367,  113,
 /*  2550 */   115,  118,  118,  367,  113,  113,  367,  119,  115,  115,
 /*  2560 */   121,  113,  115,  367,  119,  367,  367,  367,  367,  367,
 /*  2570 */   367,  115,  112,  115,  119,  367,  113,  112,  367,  367,
 /*  2580 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2590 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2600 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2610 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2620 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2630 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2640 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2650 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2660 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2670 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2680 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2690 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2700 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2710 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2720 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2730 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2740 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2750 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2760 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
 /*  2770 */   367,  367,  367,  367,  367,  367,  367,  367,  367,  367,
};
#define YY_SHIFT_COUNT    (716)
#define YY_SHIFT_MIN      (0)
#define YY_SHIFT_MAX      (2537)
static const unsigned short int yy_shift_ofst[] = {
 /*     0 */  1122, 1105, 1433, 1262, 1262,  460, 1322, 1500, 1580, 1797,
 /*    10 */  1797, 1797, 1797,  249,   21, 1797, 1797, 1797, 1797, 1797,
 /*    20 */  1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797,
 /*    30 */  1797,  256,  986,  256,  460,  460,  460,  460,  460,  460,
 /*    40 */   460,  460,  460,    0,    0,  142,  947, 1652, 1689, 1761,
 /*    50 */  1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797,
 /*    60 */  1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797,
 /*    70 */  1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797,
 /*    80 */  1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797,
 /*    90 */  1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797, 1797,
 /*   100 */  1797, 1797, 1247, 1247,  450,  450,  704,  879,  256,  256,
 /*   110 */   256,  256,  256,  256,  256,  256,  989,  989,  989,  989,
 /*   120 */    23,  120,  225,  250,  275,  368,  398,  421,  515,  538,
 /*   130 */   561,  655,  678,  701,  805,  828,  851,  947,  947,  947,
 /*   140 */   947,  947,  947,  947,  947,  947,  947,  947,  947,  947,
 /*   150 */   947,  947,  947,  947,  947,  947,  969,  947,  985, 1000,
 /*   160 */  1000,   35, 1368, 1368, 1368, 1368, 1368, 1368, 1368,  105,
 /*   170 */   128,  165,  253,  372,  256,  256,  256,  256,  256,  256,
 /*   180 */   256,  256,  256, 1090, 1274,  256,  256,  256,  377,  377,
 /*   190 */  1038,  452,   18,  242,  242,  242,  796,  166,  166, 2578,
 /*   200 */  2578, 1086, 1086, 1086,  253,  253,  677,  707,  707,  707,
 /*   210 */   707, 1111, 1111,  372,  978,  417,  674,  256,  256,  256,
 /*   220 */   256,  256,  256,  256,  256,  256,  256,  256,  256,  256,
 /*   230 */   256,  312,  256, 1584, 1584,  256,  626,  996,  996, 1222,
 /*   240 */  1432, 1432, 1176, 1278, 1176,   25,  255, 2578, 2578, 2578,
 /*   250 */  2578, 2578, 2578, 2578, 1087, 1332, 1332,  804, 1397,  829,
 /*   260 */  1201, 1230,  231, 1036, 1211,  256,  256,  256,  256,  256,
 /*   270 */   256,  256,  256,  256,  256,  256,  256,  256,  256,  256,
 /*   280 */   256,  256,  256,  256,  256,  256,  175,  256,  256,  256,
 /*   290 */   256,  256,  256,  256,  256,  256,  256,  256, 1575, 1575,
 /*   300 */  1575,  256,  256,  256,  256,  534,  256,  256, 1803, 1499,
 /*   310 */   256,  256, 1415,  256,  256,  236, 1420, 1420, 1127,  706,
 /*   320 */   334, 1624, 1420, 1420, 1420, 1218, 1420,  844,  844, 1729,
 /*   330 */   844,  687, 1558, 1291, 1472, 1352,  185, 1677, 1004, 1677,
 /*   340 */  1542, 1616,  185,  185, 1616,  185, 1004, 1542, 1718,  943,
 /*   350 */  1620, 1571,  732, 1758, 1758, 1758, 1758, 1540, 1810, 1810,
 /*   360 */  1571, 1571, 1734,   68, 2133, 2186,  255, 2115, 2137, 2033,
 /*   370 */  2137, 2116, 2201, 2201,  255, 2115, 2077, 2101, 2236, 2236,
 /*   380 */  2116, 2116, 2116, 2194, 2194, 2099, 2099, 2099, 2221, 2221,
 /*   390 */  2172, 2172, 2172, 2172, 2117, 2157, 2110, 2165, 2258, 2110,
 /*   400 */  2165, 2116, 2195, 2140, 2116, 2195, 2140, 2117, 2117, 2140,
 /*   410 */  2157, 2258, 2140, 2258, 2187, 2116, 2195, 2175, 2263, 2116,
 /*   420 */  2195, 2116, 2195, 2187, 2207, 2207, 2207, 2271, 2205, 2205,
 /*   430 */  2187, 2207, 2183, 2207, 2271, 2207, 2207, 2210, 2116, 2110,
 /*   440 */  2165, 2110, 2173, 2222, 2140, 2261, 2261, 2273, 2273, 2229,
 /*   450 */  2116, 2245, 2245, 2223, 2237, 2187, 2188, 2578, 2578, 2578,
 /*   460 */  2578, 2578, 2578, 2578, 2578, 2578, 2578, 2578, 2578, 2578,
 /*   470 */  2578, 2578, 2578, 2578, 2578, 2578, 2578, 2578, 2578, 2578,
 /*   480 */   614, 1485,  387, 1644,  490, 1714,  119, 1188,  632, 1135,
 /*   490 */   348, 1720, 1722, 1766, 1775, 1813, 1819, 1741,  964, 1845,
 /*   500 */  1848, 1522, 1561, 1882, 1570, 1884, 1886, 1901, 1908, 1913,
 /*   510 */  1680, 1728, 1586, 1840, 1938, 1863, 1839, 1944, 1822, 1967,
 /*   520 */  1969, 1973, 1887, 1866, 1907, 1978, 1995, 1897, 1754, 2332,
 /*   530 */  2333, 2265, 2192, 2197, 2353, 2334, 2276, 2274, 2350, 2202,
 /*   540 */  2277, 2354, 2355, 2356, 2279, 2286, 2360, 2288, 2296, 2363,
 /*   550 */  2247, 2292, 2215, 2293, 2289, 2297, 2294, 2291, 2299, 2361,
 /*   560 */  2214, 2298, 2219, 2300, 2302, 2382, 2305, 2304, 2303, 2306,
 /*   570 */  2307, 2383, 2308, 2309, 2310, 2315, 2384, 2312, 2392, 2320,
 /*   580 */  2394, 2246, 2378, 2325, 2323, 2324, 2283, 2284, 2287, 2311,
 /*   590 */  2335, 2290, 2322, 2301, 2313, 2314, 2330, 2331, 2331, 2326,
 /*   600 */  2285, 2319, 2339, 2348, 2316, 2343, 2329, 2317, 2331, 2318,
 /*   610 */  2416, 2450, 2331, 2321, 2429, 2430, 2431, 2432, 2327, 2362,
 /*   620 */  2459, 2357, 2365, 2464, 2358, 2421, 2364, 2422, 2418, 2462,
 /*   630 */  2366, 2374, 2359, 2457, 2340, 2349, 2373, 2437, 2376, 2377,
 /*   640 */  2379, 2380, 2440, 2452, 2381, 2410, 2494, 2386, 2446, 2488,
 /*   650 */  2385, 2387, 2389, 2390, 2399, 2492, 2395, 2345, 2393, 2498,
 /*   660 */  2397, 2400, 2398, 2396, 2391, 2401, 2508, 2404, 2406, 2405,
 /*   670 */  2372, 2409, 2408, 2515, 2516, 2517, 2451, 2467, 2454, 2507,
 /*   680 */  2471, 2453, 2414, 2524, 2419, 2393, 2423, 2424, 2425, 2449,
 /*   690 */  2427, 2426, 2428, 2433, 2415, 2420, 2434, 2435, 2436, 2438,
 /*   700 */  2519, 2441, 2443, 2442, 2439, 2444, 2448, 2447, 2445, 2455,
 /*   710 */  2456, 2458, 2463, 2536, 2537, 2460, 2465,
};
#define YY_REDUCE_COUNT (479)
#define YY_REDUCE_MIN   (-285)
#define YY_REDUCE_MAX   (2161)
static const short yy_reduce_ofst[] = {
 /*     0 */  1694,  976,  399, 1023, 1160, -214, -144,  900,  929,  -23,
 /*    10 */  1048, 1297, 1300,  959, -205,  270,  754,  945, 1049, 1066,
 /*    20 */  1107, 1191, 1270, 1312, 1306, 1345, 1359, 1106, 1364, 1387,
 /*    30 */  1449,   95, -211,  933, -157,  184,  214,  261,  282,  354,
 /*    40 */   422,  445,  471, 1120, 1264, -136, 1308, 1369, 1467, 1490,
 /*    50 */  1491, 1509, 1517, 1532, 1533, 1565, 1625, 1626, 1669, 1672,
 /*    60 */  1725, 1732, 1733, 1748, 1749, 1752, 1764, 1767, 1772, 1773,
 /*    70 */  1779, 1788, 1814, 1832, 1833, 1838, 1841, 1842, 1859, 1860,
 /*    80 */  1867, 1875, 1895, 1896, 1899, 1900, 1903, 1915, 1919, 1920,
 /*    90 */  1923, 1927, 1939, 1942, 1956, 1959, 1962, 1979, 1982, 1983,
 /*   100 */  1986, 1987, -202,  602,  564,  607,  605, 1235, 1170, -203,
 /*   110 */   671, 1427, 1564, 1279, 1740, 1296,  498,  670,  498,  670,
 /*   120 */  -221, -221, -221, -221, -221, -221, -221, -221, -221, -221,
 /*   130 */  -221, -221, -221, -221, -221, -221, -221, -221, -221, -221,
 /*   140 */  -221, -221, -221, -221, -221, -221, -221, -221, -221, -221,
 /*   150 */  -221, -221, -221, -221, -221, -221, -221, -221, -221, -221,
 /*   160 */  -221, -221, -221, -221, -221, -221, -221, -221, -221, -221,
 /*   170 */  -221, -221,  699, -198, -170,  990, 1219, 1395, 1431, 1450,
 /*   180 */  1460, 1508, 1562,  988,   55,  478, 1563,  409,  901,  977,
 /*   190 */  -126, -221,  857, 1070, 1281, 1344,  771, -221, -221, -221,
 /*   200 */  -221, -235, -235, -235,  408,  606, -178, -131,  112,  136,
 /*   210 */   164,  -70,  482, -215, -250, -285, -285,  289,  263, 1060,
 /*   220 */  1179, 1239,  322, -175,  591,  228, 1009,  722, 1018,  673,
 /*   230 */  1142,  573, 1043, 1101, 1155,  915,  899, 1216, 1303,  765,
 /*   240 */  -201, 1130, 1425, 1271, 1428,  813, 1353, 1469, 1537, 1593,
 /*   250 */  1595, 1602, -220, 1607, -256,  216,  330,  381,  384,  448,
 /*   260 */   480,  485,  588,  603,  649,  691,  742,  895, 1103, 1131,
 /*   270 */  1304, 1319, 1329, 1340, 1384, 1482, 1511, 1527, 1539, 1576,
 /*   280 */  1588, 1601, 1611, 1642, 1657, 1673, 1635, 1723, 1750, 1755,
 /*   290 */  1769, 1774, 1784, 1789, 1808, 1815, 1816, 1818, 1159, 1335,
 /*   300 */  1717, 1823, 1824, 1835, 1836, 1763, 1850, 1851, 1770, 1743,
 /*   310 */  1855, 1857, 1800, 1858,  588, 1751, 1798, 1826, 1809, 1805,
 /*   320 */  1828, 1904, 1843, 1846, 1847, 1763, 1849, 1924, 1924, 1940,
 /*   330 */  1924, 1806, 1801, 1795, 1820, 1872, 1894, 1898, 1893, 1902,
 /*   340 */  1864, 1914, 1910, 1911, 1917, 1931, 1926, 1869, 1953, 1960,
 /*   350 */  1974, 1950, 1961, 1916, 1918, 1921, 1922, 1932, 1937, 1943,
 /*   360 */  1964, 1965, 1993, 2005, 1861, 1883, 1888, 1889, 1881, 1874,
 /*   370 */  1881, 2016, 1905, 1906, 1909, 1912, 1925, 1930, 1941, 1945,
 /*   380 */  2028, 2031, 2032, 1968, 1970, 1972, 1975, 1976, 1977, 1980,
 /*   390 */  1985, 1988, 1989, 1990, 1994, 1999, 1998, 2006, 2013, 2007,
 /*   400 */  2009, 2063, 2034, 2008, 2067, 2037, 2010, 2019, 2021, 2012,
 /*   410 */  2017, 2029, 2014, 2030, 2042, 2080, 2050, 1997, 2004, 2086,
 /*   420 */  2056, 2088, 2058, 2053, 2072, 2073, 2074, 2081, 2075, 2076,
 /*   430 */  2061, 2079, 2092, 2084, 2089, 2087, 2090, 2078, 2108, 2055,
 /*   440 */  2059, 2060, 2041, 2044, 2057, 2015, 2018, 2062, 2064, 2119,
 /*   450 */  2125, 2091, 2093, 2121, 2095, 2094, 2022, 2035, 2036, 2040,
 /*   460 */  2000, 2020, 2023, 2024, 2026, 2027, 2025, 2038, 2120, 2141,
 /*   470 */  2148, 2149, 2150, 2126, 2135, 2138, 2142, 2145, 2136, 2161,
};
static const YYACTIONTYPE yy_default[] = {
 /*     0 */  1645, 1645, 1645, 1699, 1505, 1793, 1505, 1505, 1505, 1505,
 /*    10 */  1699, 1699, 1699, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*    20 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1564, 1505, 1505,
 /*    30 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*    40 */  1505, 1505, 1505, 1722, 1722, 1856, 1771, 1505, 1505, 1505,
 /*    50 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*    60 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*    70 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*    80 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*    90 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   100 */  1505, 1505, 1505, 1505, 1901, 1901, 1505, 1505, 1505, 1505,
 /*   110 */  1505, 1505, 1505, 1505, 1505, 1505, 1647, 1646, 1505, 1505,
 /*   120 */  1789, 1505, 1505, 1505, 1666, 1505, 1505, 1505, 1505, 1505,
 /*   130 */  1505, 1700, 1701, 1505, 1505, 1505, 1505, 1976, 1860, 1853,
 /*   140 */  1857, 1672, 1671, 1670, 1669, 1821, 1803, 1781, 1785, 1791,
 /*   150 */  1790, 1700, 1560, 1561, 1559, 1563, 1505, 1701, 1691, 1697,
 /*   160 */  1690, 1556, 1550, 1549, 1548, 1689, 1557, 1553, 1547, 1688,
 /*   170 */  1692, 1686, 1505, 1569, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   180 */  1505, 1505, 1505, 1873, 1625, 1505, 1505, 1505, 1505, 1505,
 /*   190 */  1703, 1687, 1771, 1704, 1517, 1515, 1505, 1694, 1693, 1696,
 /*   200 */  1695, 1742, 1575, 1574, 1505, 1505, 1861, 1505, 1505, 1505,
 /*   210 */  1505, 1505, 1505, 1505, 1901, 1505, 1505, 1505, 1505, 1505,
 /*   220 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   230 */  1505, 1805, 1505, 1901, 1901, 1505, 1771, 1901, 1901, 1947,
 /*   240 */  1663, 1663, 1520, 1786, 1520, 1941, 1927, 1884, 1770, 1770,
 /*   250 */  1770, 1770, 1793, 1770, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   260 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   270 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1850, 1848, 1505,
 /*   280 */  1755, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   290 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   300 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1774, 1505,
 /*   310 */  1505, 1505, 1505, 1505, 1505, 1505, 1774, 1774, 1750, 1505,
 /*   320 */  1814, 1629, 1774, 1774, 1774, 1779, 1774, 1631, 1630, 1777,
 /*   330 */  1763, 1505, 1505, 1505, 1871, 1744, 1675, 1665, 1778, 1665,
 /*   340 */  1826, 1780, 1675, 1675, 1780, 1675, 1778, 1826, 1596, 1619,
 /*   350 */  1589, 1722, 1505, 1805, 1805, 1805, 1805, 1778, 1786, 1786,
 /*   360 */  1722, 1722, 1562, 1777, 1505, 1505, 1505, 1933, 1974, 1971,
 /*   370 */  1973, 1541, 1505, 1505, 1505, 1933, 1935, 1861, 1855, 1855,
 /*   380 */  1541, 1541, 1541, 1758, 1758, 1754, 1754, 1754, 1744, 1744,
 /*   390 */  1734, 1734, 1734, 1734, 1682, 1673, 1788, 1786, 1654, 1788,
 /*   400 */  1786, 1541, 1868, 1780, 1541, 1868, 1780, 1682, 1682, 1780,
 /*   410 */  1673, 1654, 1780, 1654, 1639, 1541, 1868, 1820, 1818, 1541,
 /*   420 */  1868, 1541, 1868, 1639, 1627, 1627, 1627, 1611, 1505, 1505,
 /*   430 */  1639, 1627, 1596, 1627, 1611, 1627, 1627, 1614, 1541, 1788,
 /*   440 */  1786, 1788, 1784, 1782, 1780, 1911, 1911, 1725, 1725, 1543,
 /*   450 */  1541, 1643, 1643, 1505, 1505, 1639, 1919, 1889, 1889, 1884,
 /*   460 */  1983, 1948, 1947, 1948, 1947, 1948, 1965, 1965, 1577, 1771,
 /*   470 */  1771, 1771, 1771, 1577, 1598, 1598, 1629, 1629, 1577, 1771,
 /*   480 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1521, 1505,
 /*   490 */  1505, 1505, 1505, 1505, 1505, 1957, 1505, 1833, 1743, 1659,
 /*   500 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   510 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1636, 1505,
 /*   520 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1527, 1505,
 /*   530 */  1863, 1987, 1505, 1505, 1505, 1505, 1879, 1505, 1505, 1505,
 /*   540 */  1505, 1505, 1505, 1505, 1942, 1505, 1505, 1505, 1505, 1505,
 /*   550 */  1505, 1505, 1505, 1505, 1928, 1505, 1505, 1972, 1505, 1505,
 /*   560 */  1505, 1964, 1505, 1963, 1959, 1505, 1505, 1505, 1505, 1505,
 /*   570 */  1930, 1505, 1505, 1505, 1929, 1505, 1505, 1505, 1505, 1925,
 /*   580 */  1505, 1505, 1505, 1505, 1505, 1505, 1749, 1505, 1505, 1505,
 /*   590 */  1505, 1505, 1505, 1505, 1505, 1505, 1660, 1667, 1668, 1505,
 /*   600 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1681, 1505,
 /*   610 */  1505, 1505, 1676, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   620 */  1505, 1824, 1505, 1505, 1505, 1505, 1817, 1816, 1505, 1505,
 /*   630 */  1731, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   640 */  1505, 1505, 1505, 1594, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   650 */  1567, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1783, 1505,
 /*   660 */  1505, 1505, 1505, 1916, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   670 */  1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505, 1505,
 /*   680 */  1505, 1505, 1787, 1505, 1505, 1698, 1505, 1505, 1505, 1505,
 /*   690 */  1505, 1878, 1505, 1877, 1505, 1505, 1505, 1505, 1505, 1712,
 /*   700 */  1505, 1505, 1505, 1505, 1531, 1505, 1505, 1505, 1528, 1505,
 /*   710 */  1505, 1505, 1505, 1505, 1505, 1505, 1505,
};
/********** End of lemon-generated parsing tables *****************************/

/* The next table maps tokens (terminal symbols) into fallback tokens.  
** If a construct like the following:
** 
**      %fallback ID X Y Z.
**
** appears in the grammar, then ID becomes a fallback token for X, Y,
** and Z.  Whenever one of the tokens X, Y, or Z is input to the parser
** but it does not parse, the type of the token is changed to ID and
** the parse is retried before an error is thrown.
**
** This feature can be used, for example, to cause some keywords in a language
** to revert to identifiers if they keyword does not apply in the context where
** it appears.
*/
#ifdef YYFALLBACK
static const YYCODETYPE yyFallback[] = {
    0,  /*          $ => nothing */
   40,  /*      ABORT => ID */
   40,  /*     ACTION => ID */
   40,  /*      AFTER => ID */
   40,  /*    ANALYZE => ID */
   40,  /*        ASC => ID */
   40,  /*     ATTACH => ID */
   40,  /*     BEFORE => ID */
   40,  /*      BEGIN => ID */
   40,  /*         BY => ID */
   40,  /*    CASCADE => ID */
   40,  /*       CAST => ID */
   40,  /*   CONFLICT => ID */
   40,  /*   DATABASE => ID */
   40,  /*   DEFERRED => ID */
   40,  /*       DESC => ID */
   40,  /*     DETACH => ID */
   40,  /*       EACH => ID */
   40,  /*        END => ID */
   40,  /*  EXCLUSIVE => ID */
   40,  /*    EXPLAIN => ID */
   40,  /*       FAIL => ID */
    0,  /*         OR => nothing */
    0,  /*        AND => nothing */
    0,  /*        NOT => nothing */
    0,  /*         IS => nothing */
    0,  /*      ISNOT => nothing */
   40,  /*      MATCH => ID */
   40,  /*    LIKE_KW => ID */
    0,  /*    BETWEEN => nothing */
    0,  /*         IN => nothing */
    0,  /*     ISNULL => nothing */
    0,  /*    NOTNULL => nothing */
    0,  /*         NE => nothing */
    0,  /*         EQ => nothing */
    0,  /*         GT => nothing */
    0,  /*         LE => nothing */
    0,  /*         LT => nothing */
    0,  /*         GE => nothing */
    0,  /*     ESCAPE => nothing */
    0,  /*         ID => nothing */
   40,  /*   COLUMNKW => ID */
   40,  /*         DO => ID */
   40,  /*        FOR => ID */
   40,  /*     IGNORE => ID */
   40,  /*  IMMEDIATE => ID */
   40,  /*  INITIALLY => ID */
   40,  /*    INSTEAD => ID */
   40,  /*         NO => ID */
   40,  /*       PLAN => ID */
   40,  /*      QUERY => ID */
   40,  /*        KEY => ID */
   40,  /*         OF => ID */
   40,  /*     OFFSET => ID */
   40,  /*     PRAGMA => ID */
   40,  /*      RAISE => ID */
   40,  /*  RECURSIVE => ID */
   40,  /*    RELEASE => ID */
   40,  /*    REPLACE => ID */
   40,  /*   RESTRICT => ID */
   40,  /*        ROW => ID */
   40,  /*       ROWS => ID */
   40,  /*   ROLLBACK => ID */
   40,  /*  SAVEPOINT => ID */
   40,  /*       TEMP => ID */
   40,  /*    TRIGGER => ID */
   40,  /*     VACUUM => ID */
   40,  /*       VIEW => ID */
   40,  /*    VIRTUAL => ID */
   40,  /*       WITH => ID */
   40,  /*    WITHOUT => ID */
   40,  /*      NULLS => ID */
   40,  /*      FIRST => ID */
   40,  /*       LAST => ID */
   40,  /*    CURRENT => ID */
   40,  /*  FOLLOWING => ID */
   40,  /*  PARTITION => ID */
   40,  /*  PRECEDING => ID */
   40,  /*      RANGE => ID */
   40,  /*  UNBOUNDED => ID */
   40,  /*    EXCLUDE => ID */
   40,  /*     GROUPS => ID */
   40,  /*     OTHERS => ID */
   40,  /*       TIES => ID */
   40,  /*  GENERATED => ID */
   40,  /*     ALWAYS => ID */
   40,  /*     WITHIN => ID */
   40,  /* MATERIALIZED => ID */
   40,  /*    REINDEX => ID */
   40,  /*     RENAME => ID */
   40,  /*   CTIME_KW => ID */
   40,  /*         IF => ID */
    0,  /*        ANY => nothing */
    0,  /*     BITAND => nothing */
    0,  /*      BITOR => nothing */
    0,  /*     LSHIFT => nothing */
    0,  /*     RSHIFT => nothing */
    0,  /*       PLUS => nothing */
    0,  /*      MINUS => nothing */
    0,  /*       STAR => nothing */
    0,  /*      SLASH => nothing */
    0,  /*        REM => nothing */
    0,  /*     CONCAT => nothing */
    0,  /*        PTR => nothing */
    0,  /*    COLLATE => nothing */
    0,  /*     BITNOT => nothing */
    0,  /*         ON => nothing */
    0,  /*    INDEXED => nothing */
    0,  /*     STRING => nothing */
    0,  /*    JOIN_KW => nothing */
    0,  /*    INTEGER => nothing */
    0,  /*      FLOAT => nothing */
    0,  /*       SEMI => nothing */
    0,  /*         LP => nothing */
    0,  /*      ORDER => nothing */
    0,  /*         RP => nothing */
    0,  /*      GROUP => nothing */
    0,  /*         AS => nothing */
    0,  /*      COMMA => nothing */
    0,  /*        DOT => nothing */
    0,  /*      UNION => nothing */
    0,  /*        ALL => nothing */
    0,  /*     EXCEPT => nothing */
    0,  /*  INTERSECT => nothing */
    0,  /*     EXISTS => nothing */
    0,  /*       NULL => nothing */
    0,  /*   DISTINCT => nothing */
    0,  /*       FROM => nothing */
    0,  /*       CASE => nothing */
    0,  /*       WHEN => nothing */
    0,  /*       THEN => nothing */
    0,  /*       ELSE => nothing */
    0,  /*      TABLE => nothing */
    0,  /* CONSTRAINT => nothing */
    0,  /*    DEFAULT => nothing */
    0,  /*    PRIMARY => nothing */
    0,  /*     UNIQUE => nothing */
    0,  /*      CHECK => nothing */
    0,  /* REFERENCES => nothing */
    0,  /*   AUTOINCR => nothing */
    0,  /*     INSERT => nothing */
    0,  /*     DELETE => nothing */
    0,  /*     UPDATE => nothing */
    0,  /*        SET => nothing */
    0,  /* DEFERRABLE => nothing */
    0,  /*    FOREIGN => nothing */
    0,  /*       INTO => nothing */
    0,  /*     VALUES => nothing */
    0,  /*      WHERE => nothing */
    0,  /*  RETURNING => nothing */
    0,  /*    NOTHING => nothing */
    0,  /*       BLOB => nothing */
    0,  /*    QNUMBER => nothing */
    0,  /*   VARIABLE => nothing */
    0,  /*       DROP => nothing */
    0,  /*      INDEX => nothing */
    0,  /*      ALTER => nothing */
    0,  /*         TO => nothing */
    0,  /*        ADD => nothing */
    0,  /*     COMMIT => nothing */
    0,  /* TRANSACTION => nothing */
    0,  /*     SELECT => nothing */
    0,  /*     HAVING => nothing */
    0,  /*      LIMIT => nothing */
    0,  /*       JOIN => nothing */
    0,  /*      USING => nothing */
    0,  /*     CREATE => nothing */
    0,  /*     WINDOW => nothing */
    0,  /*       OVER => nothing */
    0,  /*     FILTER => nothing */
    0,  /*     COLUMN => nothing */
    0,  /* AGG_FUNCTION => nothing */
    0,  /* AGG_COLUMN => nothing */
    0,  /*  TRUEFALSE => nothing */
   40,  /*   FUNCTION => ID */
    0,  /*      UPLUS => nothing */
    0,  /*     UMINUS => nothing */
    0,  /*      TRUTH => nothing */
    0,  /*   REGISTER => nothing */
    0,  /*     VECTOR => nothing */
    0,  /* SELECT_COLUMN => nothing */
    0,  /* IF_NULL_ROW => nothing */
    0,  /*   ASTERISK => nothing */
    0,  /*       SPAN => nothing */
    0,  /*      ERROR => nothing */
    0,  /*      SPACE => nothing */
    0,  /*    COMMENT => nothing */
    0,  /*    ILLEGAL => nothing */
    0,  /*       BANG => nothing */
   40,  /*   PERFETTO => ID */
   40,  /*     MODULE => ID */
   40,  /*    RETURNS => ID */
   40,  /*      MACRO => ID */
   40,  /*  DELEGATES => ID */
   40,  /*    INCLUDE => ID */
   40,  /*       TREE => ID */
   40,  /* ACCUMULATE => ID */
   40,  /*         UP => ID */
   40,  /*       DOWN => ID */
   40,  /*   INTERVAL => ID */
   40,  /* INTERSECTION => ID */
   40,  /*        PER => ID */
};
#endif /* YYFALLBACK */

/* The following structure represents a single element of the
** parser's stack.  Information stored includes:
**
**   +  The state number for the parser at this level of the stack.
**
**   +  The value of the token stored at this level of the stack.
**      (In other words, the "major" token.)
**
**   +  The semantic value stored at this level of the stack.  This is
**      the information used by the action routines in the grammar.
**      It is sometimes called the "minor" token.
**
** After the "shift" half of a SHIFTREDUCE action, the stateno field
** actually contains the reduce action for the second half of the
** SHIFTREDUCE.
*/
struct yyStackEntry {
  YYACTIONTYPE stateno;  /* The state-number, or reduce action in SHIFTREDUCE */
  YYCODETYPE major;      /* The major token value.  This is the code
                         ** number for the token at this stack level */
  YYMINORTYPE minor;     /* The user-supplied minor token value.  This
                         ** is the value of the token  */
};
typedef struct yyStackEntry yyStackEntry;

/* The state of the parser is completely contained in an instance of
** the following structure */
struct yyParser {
  yyStackEntry *yytos;          /* Pointer to top element of the stack */
#ifdef YYTRACKMAXSTACKDEPTH
  int yyhwm;                    /* High-water mark of the stack */
#endif
#ifndef YYNOERRORRECOVERY
  int yyerrcnt;                 /* Shifts left before out of the error */
#endif
  SynqPerfettoParseARG_SDECL                /* A place to hold %extra_argument */
  SynqPerfettoParseCTX_SDECL                /* A place to hold %extra_context */
  yyStackEntry *yystackEnd;           /* Last entry in the stack */
  yyStackEntry *yystack;              /* The parser stack */
  yyStackEntry yystk0[YYSTACKDEPTH];  /* Initial stack space */
};
typedef struct yyParser yyParser;

#include <assert.h>
#ifndef NDEBUG
#include <stdio.h>



static FILE *yyTraceFILE = 0;
static char *yyTracePrompt = 0;
#endif /* NDEBUG */

#ifndef NDEBUG
/* 
** Turn parser tracing on by giving a stream to which to write the trace
** and a prompt to preface each trace message.  Tracing is turned off
** by making either argument NULL 
**
** Inputs:
** <ul>
** <li> A FILE* to which trace output should be written.
**      If NULL, then tracing is turned off.
** <li> A prefix string written at the beginning of every
**      line of trace output.  If NULL, then tracing is
**      turned off.
** </ul>
**
** Outputs:
** None.
*/
void SynqPerfettoParseTrace(FILE *TraceFILE, char *zTracePrompt){
  yyTraceFILE = TraceFILE;
  yyTracePrompt = zTracePrompt;
  if( yyTraceFILE==0 ) yyTracePrompt = 0;
  else if( yyTracePrompt==0 ) yyTraceFILE = 0;
}
#endif /* NDEBUG */

#if defined(YYCOVERAGE) || !defined(NDEBUG)
/* For tracing shifts, the names of all terminals and nonterminals
** are required.  The following table supplies these names */
static const char *const yyTokenName[] = { 
  /*    0 */ "$",
  /*    1 */ "ABORT",
  /*    2 */ "ACTION",
  /*    3 */ "AFTER",
  /*    4 */ "ANALYZE",
  /*    5 */ "ASC",
  /*    6 */ "ATTACH",
  /*    7 */ "BEFORE",
  /*    8 */ "BEGIN",
  /*    9 */ "BY",
  /*   10 */ "CASCADE",
  /*   11 */ "CAST",
  /*   12 */ "CONFLICT",
  /*   13 */ "DATABASE",
  /*   14 */ "DEFERRED",
  /*   15 */ "DESC",
  /*   16 */ "DETACH",
  /*   17 */ "EACH",
  /*   18 */ "END",
  /*   19 */ "EXCLUSIVE",
  /*   20 */ "EXPLAIN",
  /*   21 */ "FAIL",
  /*   22 */ "OR",
  /*   23 */ "AND",
  /*   24 */ "NOT",
  /*   25 */ "IS",
  /*   26 */ "ISNOT",
  /*   27 */ "MATCH",
  /*   28 */ "LIKE_KW",
  /*   29 */ "BETWEEN",
  /*   30 */ "IN",
  /*   31 */ "ISNULL",
  /*   32 */ "NOTNULL",
  /*   33 */ "NE",
  /*   34 */ "EQ",
  /*   35 */ "GT",
  /*   36 */ "LE",
  /*   37 */ "LT",
  /*   38 */ "GE",
  /*   39 */ "ESCAPE",
  /*   40 */ "ID",
  /*   41 */ "COLUMNKW",
  /*   42 */ "DO",
  /*   43 */ "FOR",
  /*   44 */ "IGNORE",
  /*   45 */ "IMMEDIATE",
  /*   46 */ "INITIALLY",
  /*   47 */ "INSTEAD",
  /*   48 */ "NO",
  /*   49 */ "PLAN",
  /*   50 */ "QUERY",
  /*   51 */ "KEY",
  /*   52 */ "OF",
  /*   53 */ "OFFSET",
  /*   54 */ "PRAGMA",
  /*   55 */ "RAISE",
  /*   56 */ "RECURSIVE",
  /*   57 */ "RELEASE",
  /*   58 */ "REPLACE",
  /*   59 */ "RESTRICT",
  /*   60 */ "ROW",
  /*   61 */ "ROWS",
  /*   62 */ "ROLLBACK",
  /*   63 */ "SAVEPOINT",
  /*   64 */ "TEMP",
  /*   65 */ "TRIGGER",
  /*   66 */ "VACUUM",
  /*   67 */ "VIEW",
  /*   68 */ "VIRTUAL",
  /*   69 */ "WITH",
  /*   70 */ "WITHOUT",
  /*   71 */ "NULLS",
  /*   72 */ "FIRST",
  /*   73 */ "LAST",
  /*   74 */ "CURRENT",
  /*   75 */ "FOLLOWING",
  /*   76 */ "PARTITION",
  /*   77 */ "PRECEDING",
  /*   78 */ "RANGE",
  /*   79 */ "UNBOUNDED",
  /*   80 */ "EXCLUDE",
  /*   81 */ "GROUPS",
  /*   82 */ "OTHERS",
  /*   83 */ "TIES",
  /*   84 */ "GENERATED",
  /*   85 */ "ALWAYS",
  /*   86 */ "WITHIN",
  /*   87 */ "MATERIALIZED",
  /*   88 */ "REINDEX",
  /*   89 */ "RENAME",
  /*   90 */ "CTIME_KW",
  /*   91 */ "IF",
  /*   92 */ "ANY",
  /*   93 */ "BITAND",
  /*   94 */ "BITOR",
  /*   95 */ "LSHIFT",
  /*   96 */ "RSHIFT",
  /*   97 */ "PLUS",
  /*   98 */ "MINUS",
  /*   99 */ "STAR",
  /*  100 */ "SLASH",
  /*  101 */ "REM",
  /*  102 */ "CONCAT",
  /*  103 */ "PTR",
  /*  104 */ "COLLATE",
  /*  105 */ "BITNOT",
  /*  106 */ "ON",
  /*  107 */ "INDEXED",
  /*  108 */ "STRING",
  /*  109 */ "JOIN_KW",
  /*  110 */ "INTEGER",
  /*  111 */ "FLOAT",
  /*  112 */ "SEMI",
  /*  113 */ "LP",
  /*  114 */ "ORDER",
  /*  115 */ "RP",
  /*  116 */ "GROUP",
  /*  117 */ "AS",
  /*  118 */ "COMMA",
  /*  119 */ "DOT",
  /*  120 */ "UNION",
  /*  121 */ "ALL",
  /*  122 */ "EXCEPT",
  /*  123 */ "INTERSECT",
  /*  124 */ "EXISTS",
  /*  125 */ "NULL",
  /*  126 */ "DISTINCT",
  /*  127 */ "FROM",
  /*  128 */ "CASE",
  /*  129 */ "WHEN",
  /*  130 */ "THEN",
  /*  131 */ "ELSE",
  /*  132 */ "TABLE",
  /*  133 */ "CONSTRAINT",
  /*  134 */ "DEFAULT",
  /*  135 */ "PRIMARY",
  /*  136 */ "UNIQUE",
  /*  137 */ "CHECK",
  /*  138 */ "REFERENCES",
  /*  139 */ "AUTOINCR",
  /*  140 */ "INSERT",
  /*  141 */ "DELETE",
  /*  142 */ "UPDATE",
  /*  143 */ "SET",
  /*  144 */ "DEFERRABLE",
  /*  145 */ "FOREIGN",
  /*  146 */ "INTO",
  /*  147 */ "VALUES",
  /*  148 */ "WHERE",
  /*  149 */ "RETURNING",
  /*  150 */ "NOTHING",
  /*  151 */ "BLOB",
  /*  152 */ "QNUMBER",
  /*  153 */ "VARIABLE",
  /*  154 */ "DROP",
  /*  155 */ "INDEX",
  /*  156 */ "ALTER",
  /*  157 */ "TO",
  /*  158 */ "ADD",
  /*  159 */ "COMMIT",
  /*  160 */ "TRANSACTION",
  /*  161 */ "SELECT",
  /*  162 */ "HAVING",
  /*  163 */ "LIMIT",
  /*  164 */ "JOIN",
  /*  165 */ "USING",
  /*  166 */ "CREATE",
  /*  167 */ "WINDOW",
  /*  168 */ "OVER",
  /*  169 */ "FILTER",
  /*  170 */ "COLUMN",
  /*  171 */ "AGG_FUNCTION",
  /*  172 */ "AGG_COLUMN",
  /*  173 */ "TRUEFALSE",
  /*  174 */ "FUNCTION",
  /*  175 */ "UPLUS",
  /*  176 */ "UMINUS",
  /*  177 */ "TRUTH",
  /*  178 */ "REGISTER",
  /*  179 */ "VECTOR",
  /*  180 */ "SELECT_COLUMN",
  /*  181 */ "IF_NULL_ROW",
  /*  182 */ "ASTERISK",
  /*  183 */ "SPAN",
  /*  184 */ "ERROR",
  /*  185 */ "SPACE",
  /*  186 */ "COMMENT",
  /*  187 */ "ILLEGAL",
  /*  188 */ "BANG",
  /*  189 */ "PERFETTO",
  /*  190 */ "MODULE",
  /*  191 */ "RETURNS",
  /*  192 */ "MACRO",
  /*  193 */ "DELEGATES",
  /*  194 */ "INCLUDE",
  /*  195 */ "TREE",
  /*  196 */ "ACCUMULATE",
  /*  197 */ "UP",
  /*  198 */ "DOWN",
  /*  199 */ "INTERVAL",
  /*  200 */ "INTERSECTION",
  /*  201 */ "PER",
  /*  202 */ "input",
  /*  203 */ "cmdlist",
  /*  204 */ "ecmd",
  /*  205 */ "cmdx",
  /*  206 */ "error",
  /*  207 */ "cmd",
  /*  208 */ "expr",
  /*  209 */ "distinct",
  /*  210 */ "exprlist",
  /*  211 */ "sortlist",
  /*  212 */ "filter_over",
  /*  213 */ "typetoken",
  /*  214 */ "typename",
  /*  215 */ "signed",
  /*  216 */ "selcollist",
  /*  217 */ "sclp",
  /*  218 */ "scanpt",
  /*  219 */ "nm",
  /*  220 */ "multiselect_op",
  /*  221 */ "in_op",
  /*  222 */ "dbnm",
  /*  223 */ "selectnowith",
  /*  224 */ "oneselect",
  /*  225 */ "select",
  /*  226 */ "paren_exprlist",
  /*  227 */ "likeop",
  /*  228 */ "between_op",
  /*  229 */ "case_operand",
  /*  230 */ "case_exprlist",
  /*  231 */ "case_else",
  /*  232 */ "scantok",
  /*  233 */ "autoinc",
  /*  234 */ "refargs",
  /*  235 */ "refarg",
  /*  236 */ "refact",
  /*  237 */ "defer_subclause",
  /*  238 */ "init_deferred_pred_opt",
  /*  239 */ "defer_subclause_opt",
  /*  240 */ "table_option_set",
  /*  241 */ "table_option",
  /*  242 */ "onconf",
  /*  243 */ "ccons",
  /*  244 */ "carglist",
  /*  245 */ "tcons",
  /*  246 */ "conslist",
  /*  247 */ "tconscomma",
  /*  248 */ "generated",
  /*  249 */ "create_table",
  /*  250 */ "create_table_args",
  /*  251 */ "createkw",
  /*  252 */ "temp",
  /*  253 */ "ifnotexists",
  /*  254 */ "columnlist",
  /*  255 */ "conslist_opt",
  /*  256 */ "columnname",
  /*  257 */ "term",
  /*  258 */ "sortorder",
  /*  259 */ "eidlist_opt",
  /*  260 */ "eidlist",
  /*  261 */ "resolvetype",
  /*  262 */ "withnm",
  /*  263 */ "wqas",
  /*  264 */ "collate",
  /*  265 */ "wqlist",
  /*  266 */ "wqitem",
  /*  267 */ "with",
  /*  268 */ "insert_cmd",
  /*  269 */ "orconf",
  /*  270 */ "indexed_opt",
  /*  271 */ "where_opt_ret",
  /*  272 */ "upsert",
  /*  273 */ "returning",
  /*  274 */ "xfullname",
  /*  275 */ "orderby_opt",
  /*  276 */ "limit_opt",
  /*  277 */ "setlist",
  /*  278 */ "from",
  /*  279 */ "idlist_opt",
  /*  280 */ "raisetype",
  /*  281 */ "indexed_by",
  /*  282 */ "idlist",
  /*  283 */ "where_opt",
  /*  284 */ "nexprlist",
  /*  285 */ "nmorerr",
  /*  286 */ "nulls",
  /*  287 */ "ifexists",
  /*  288 */ "transtype",
  /*  289 */ "trans_opt",
  /*  290 */ "savepoint_opt",
  /*  291 */ "kwcolumn_opt",
  /*  292 */ "fullname",
  /*  293 */ "add_column_fullname",
  /*  294 */ "as",
  /*  295 */ "groupby_opt",
  /*  296 */ "having_opt",
  /*  297 */ "window_clause",
  /*  298 */ "seltablist",
  /*  299 */ "on_using",
  /*  300 */ "joinop",
  /*  301 */ "stl_prefix",
  /*  302 */ "trigger_time",
  /*  303 */ "foreach_clause",
  /*  304 */ "trnm",
  /*  305 */ "trigger_decl",
  /*  306 */ "trigger_cmd_list",
  /*  307 */ "trigger_event",
  /*  308 */ "when_clause",
  /*  309 */ "trigger_cmd",
  /*  310 */ "tridxby",
  /*  311 */ "database_kw_opt",
  /*  312 */ "plus_num",
  /*  313 */ "minus_num",
  /*  314 */ "nmnum",
  /*  315 */ "uniqueflag",
  /*  316 */ "explain",
  /*  317 */ "key_opt",
  /*  318 */ "vinto",
  /*  319 */ "values",
  /*  320 */ "mvalues",
  /*  321 */ "create_vtab",
  /*  322 */ "vtabarglist",
  /*  323 */ "vtabarg",
  /*  324 */ "vtabargtoken",
  /*  325 */ "lp",
  /*  326 */ "anylist",
  /*  327 */ "range_or_rows",
  /*  328 */ "frame_exclude_opt",
  /*  329 */ "frame_exclude",
  /*  330 */ "windowdefn_list",
  /*  331 */ "windowdefn",
  /*  332 */ "window",
  /*  333 */ "frame_opt",
  /*  334 */ "frame_bound_s",
  /*  335 */ "frame_bound_e",
  /*  336 */ "frame_bound",
  /*  337 */ "filter_clause",
  /*  338 */ "over_clause",
  /*  339 */ "perfetto_or_replace",
  /*  340 */ "perfetto_arg_type",
  /*  341 */ "perfetto_arg_def_list",
  /*  342 */ "perfetto_arg_def_list_ne",
  /*  343 */ "perfetto_table_schema",
  /*  344 */ "perfetto_table_impl",
  /*  345 */ "perfetto_return_type",
  /*  346 */ "perfetto_indexed_col_list",
  /*  347 */ "perfetto_macro_arg_list",
  /*  348 */ "perfetto_macro_arg_list_ne",
  /*  349 */ "perfetto_module_name",
  /*  350 */ "select_body_start",
  /*  351 */ "select_body_end",
  /*  352 */ "perfetto_pipe",
  /*  353 */ "perfetto_pipe_source",
  /*  354 */ "perfetto_tree_direction",
  /*  355 */ "perfetto_tree_aggregate",
  /*  356 */ "perfetto_tree_aggregate_list",
  /*  357 */ "perfetto_pipe_select_item",
  /*  358 */ "perfetto_pipe_select_list",
  /*  359 */ "perfetto_pipe_stage",
  /*  360 */ "perfetto_pipe_stage_list",
  /*  361 */ "perfetto_pipe_source_list",
  /*  362 */ "perfetto_per_col_list",
  /*  363 */ "perfetto_per",
  /*  364 */ "perfetto_pipeline",
  /*  365 */ "before_macro_body",
  /*  366 */ "perfetto_macro_body",
};
#endif /* defined(YYCOVERAGE) || !defined(NDEBUG) */

#ifndef NDEBUG
/* For tracing reduce actions, the names of all rules are required.
*/
static const char *const yyRuleName[] = {
 /*   0 */ "input ::= cmdlist",
 /*   1 */ "cmdlist ::= cmdlist ecmd",
 /*   2 */ "cmdlist ::= ecmd",
 /*   3 */ "ecmd ::= SEMI",
 /*   4 */ "ecmd ::= cmdx SEMI",
 /*   5 */ "ecmd ::= error SEMI",
 /*   6 */ "cmdx ::= cmd",
 /*   7 */ "expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist ORDER BY sortlist RP",
 /*   8 */ "expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist ORDER BY sortlist RP filter_over",
 /*   9 */ "expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP WITHIN GROUP LP ORDER BY expr RP",
 /*  10 */ "expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP WITHIN GROUP LP ORDER BY expr RP filter_over",
 /*  11 */ "expr ::= CAST LP expr AS typetoken RP",
 /*  12 */ "typetoken ::=",
 /*  13 */ "typetoken ::= typename",
 /*  14 */ "typetoken ::= typename LP signed RP",
 /*  15 */ "typetoken ::= typename LP signed COMMA signed RP",
 /*  16 */ "typename ::= ID|STRING",
 /*  17 */ "typename ::= typename ID|STRING",
 /*  18 */ "selcollist ::= sclp scanpt nm DOT STAR",
 /*  19 */ "expr ::= ID|INDEXED|JOIN_KW",
 /*  20 */ "expr ::= nm DOT nm",
 /*  21 */ "expr ::= nm DOT nm DOT nm",
 /*  22 */ "selectnowith ::= selectnowith multiselect_op oneselect",
 /*  23 */ "multiselect_op ::= UNION",
 /*  24 */ "multiselect_op ::= UNION ALL",
 /*  25 */ "multiselect_op ::= EXCEPT|INTERSECT",
 /*  26 */ "expr ::= LP select RP",
 /*  27 */ "expr ::= EXISTS LP select RP",
 /*  28 */ "in_op ::= IN",
 /*  29 */ "in_op ::= NOT IN",
 /*  30 */ "expr ::= expr in_op LP exprlist RP",
 /*  31 */ "expr ::= expr in_op LP select RP",
 /*  32 */ "expr ::= expr in_op nm dbnm paren_exprlist",
 /*  33 */ "dbnm ::=",
 /*  34 */ "dbnm ::= DOT nm",
 /*  35 */ "paren_exprlist ::=",
 /*  36 */ "paren_exprlist ::= LP exprlist RP",
 /*  37 */ "expr ::= expr ISNULL|NOTNULL",
 /*  38 */ "expr ::= expr NOT NULL",
 /*  39 */ "expr ::= expr IS expr",
 /*  40 */ "expr ::= expr IS NOT expr",
 /*  41 */ "expr ::= expr IS NOT DISTINCT FROM expr",
 /*  42 */ "expr ::= expr IS DISTINCT FROM expr",
 /*  43 */ "between_op ::= BETWEEN",
 /*  44 */ "between_op ::= NOT BETWEEN",
 /*  45 */ "expr ::= expr between_op expr AND expr",
 /*  46 */ "likeop ::= LIKE_KW|MATCH",
 /*  47 */ "likeop ::= NOT LIKE_KW|MATCH",
 /*  48 */ "expr ::= expr likeop expr",
 /*  49 */ "expr ::= expr likeop expr ESCAPE expr",
 /*  50 */ "expr ::= CASE case_operand case_exprlist case_else END",
 /*  51 */ "case_exprlist ::= case_exprlist WHEN expr THEN expr",
 /*  52 */ "case_exprlist ::= WHEN expr THEN expr",
 /*  53 */ "case_else ::= ELSE expr",
 /*  54 */ "case_else ::=",
 /*  55 */ "case_operand ::= expr",
 /*  56 */ "case_operand ::=",
 /*  57 */ "cmd ::= create_table create_table_args",
 /*  58 */ "create_table ::= createkw temp TABLE ifnotexists nm dbnm",
 /*  59 */ "create_table_args ::= LP columnlist conslist_opt RP table_option_set",
 /*  60 */ "create_table_args ::= AS select",
 /*  61 */ "table_option_set ::=",
 /*  62 */ "table_option_set ::= table_option",
 /*  63 */ "table_option_set ::= table_option_set COMMA table_option",
 /*  64 */ "table_option ::= WITHOUT nm",
 /*  65 */ "table_option ::= nm",
 /*  66 */ "columnlist ::= columnlist COMMA columnname carglist",
 /*  67 */ "columnlist ::= columnname carglist",
 /*  68 */ "carglist ::= carglist ccons",
 /*  69 */ "carglist ::=",
 /*  70 */ "ccons ::= CONSTRAINT nm",
 /*  71 */ "ccons ::= DEFAULT scantok term",
 /*  72 */ "ccons ::= DEFAULT LP expr RP",
 /*  73 */ "ccons ::= DEFAULT PLUS scantok term",
 /*  74 */ "ccons ::= DEFAULT MINUS scantok term",
 /*  75 */ "ccons ::= DEFAULT scantok ID|INDEXED",
 /*  76 */ "ccons ::= NULL onconf",
 /*  77 */ "ccons ::= NOT NULL onconf",
 /*  78 */ "ccons ::= PRIMARY KEY sortorder onconf autoinc",
 /*  79 */ "ccons ::= UNIQUE onconf",
 /*  80 */ "ccons ::= CHECK LP expr RP",
 /*  81 */ "ccons ::= REFERENCES nm eidlist_opt refargs",
 /*  82 */ "ccons ::= defer_subclause",
 /*  83 */ "ccons ::= COLLATE ID|STRING",
 /*  84 */ "ccons ::= GENERATED ALWAYS AS generated",
 /*  85 */ "ccons ::= AS generated",
 /*  86 */ "generated ::= LP expr RP",
 /*  87 */ "generated ::= LP expr RP ID",
 /*  88 */ "autoinc ::=",
 /*  89 */ "autoinc ::= AUTOINCR",
 /*  90 */ "refargs ::=",
 /*  91 */ "refargs ::= refargs refarg",
 /*  92 */ "refarg ::= MATCH nm",
 /*  93 */ "refarg ::= ON INSERT refact",
 /*  94 */ "refarg ::= ON DELETE refact",
 /*  95 */ "refarg ::= ON UPDATE refact",
 /*  96 */ "refact ::= SET NULL",
 /*  97 */ "refact ::= SET DEFAULT",
 /*  98 */ "refact ::= CASCADE",
 /*  99 */ "refact ::= RESTRICT",
 /* 100 */ "refact ::= NO ACTION",
 /* 101 */ "defer_subclause ::= NOT DEFERRABLE init_deferred_pred_opt",
 /* 102 */ "defer_subclause ::= DEFERRABLE init_deferred_pred_opt",
 /* 103 */ "init_deferred_pred_opt ::=",
 /* 104 */ "init_deferred_pred_opt ::= INITIALLY DEFERRED",
 /* 105 */ "init_deferred_pred_opt ::= INITIALLY IMMEDIATE",
 /* 106 */ "conslist_opt ::=",
 /* 107 */ "conslist_opt ::= COMMA conslist",
 /* 108 */ "conslist ::= conslist tconscomma tcons",
 /* 109 */ "conslist ::= tcons",
 /* 110 */ "tconscomma ::= COMMA",
 /* 111 */ "tconscomma ::=",
 /* 112 */ "tcons ::= CONSTRAINT nm",
 /* 113 */ "tcons ::= PRIMARY KEY LP sortlist autoinc RP onconf",
 /* 114 */ "tcons ::= UNIQUE LP sortlist RP onconf",
 /* 115 */ "tcons ::= CHECK LP expr RP onconf",
 /* 116 */ "tcons ::= FOREIGN KEY LP eidlist RP REFERENCES nm eidlist_opt refargs defer_subclause_opt",
 /* 117 */ "defer_subclause_opt ::=",
 /* 118 */ "defer_subclause_opt ::= defer_subclause",
 /* 119 */ "onconf ::=",
 /* 120 */ "onconf ::= ON CONFLICT resolvetype",
 /* 121 */ "scantok ::=",
 /* 122 */ "select ::= WITH wqlist selectnowith",
 /* 123 */ "select ::= WITH RECURSIVE wqlist selectnowith",
 /* 124 */ "wqitem ::= withnm eidlist_opt wqas LP select RP",
 /* 125 */ "wqlist ::= wqitem",
 /* 126 */ "wqlist ::= wqlist COMMA wqitem",
 /* 127 */ "withnm ::= nm",
 /* 128 */ "wqas ::= AS",
 /* 129 */ "wqas ::= AS MATERIALIZED",
 /* 130 */ "wqas ::= AS NOT MATERIALIZED",
 /* 131 */ "eidlist_opt ::=",
 /* 132 */ "eidlist_opt ::= LP eidlist RP",
 /* 133 */ "eidlist ::= nm collate sortorder",
 /* 134 */ "eidlist ::= eidlist COMMA nm collate sortorder",
 /* 135 */ "collate ::=",
 /* 136 */ "collate ::= COLLATE ID|STRING",
 /* 137 */ "with ::=",
 /* 138 */ "with ::= WITH wqlist",
 /* 139 */ "with ::= WITH RECURSIVE wqlist",
 /* 140 */ "cmd ::= with DELETE FROM xfullname indexed_opt where_opt_ret orderby_opt limit_opt",
 /* 141 */ "cmd ::= with UPDATE orconf xfullname indexed_opt SET setlist from where_opt_ret orderby_opt limit_opt",
 /* 142 */ "cmd ::= with insert_cmd INTO xfullname idlist_opt select upsert",
 /* 143 */ "cmd ::= with insert_cmd INTO xfullname idlist_opt DEFAULT VALUES returning",
 /* 144 */ "insert_cmd ::= INSERT orconf",
 /* 145 */ "insert_cmd ::= REPLACE",
 /* 146 */ "orconf ::=",
 /* 147 */ "orconf ::= OR resolvetype",
 /* 148 */ "resolvetype ::= raisetype",
 /* 149 */ "resolvetype ::= IGNORE",
 /* 150 */ "resolvetype ::= REPLACE",
 /* 151 */ "xfullname ::= nm",
 /* 152 */ "xfullname ::= nm DOT nm",
 /* 153 */ "xfullname ::= nm DOT nm AS nm",
 /* 154 */ "xfullname ::= nm AS nm",
 /* 155 */ "indexed_opt ::=",
 /* 156 */ "indexed_opt ::= indexed_by",
 /* 157 */ "where_opt_ret ::=",
 /* 158 */ "where_opt_ret ::= WHERE expr",
 /* 159 */ "where_opt_ret ::= RETURNING selcollist",
 /* 160 */ "where_opt_ret ::= WHERE expr RETURNING selcollist",
 /* 161 */ "setlist ::= setlist COMMA nm EQ expr",
 /* 162 */ "setlist ::= setlist COMMA LP idlist RP EQ expr",
 /* 163 */ "setlist ::= nm EQ expr",
 /* 164 */ "setlist ::= LP idlist RP EQ expr",
 /* 165 */ "idlist_opt ::=",
 /* 166 */ "idlist_opt ::= LP idlist RP",
 /* 167 */ "upsert ::=",
 /* 168 */ "upsert ::= RETURNING selcollist",
 /* 169 */ "upsert ::= ON CONFLICT LP sortlist RP where_opt DO UPDATE SET setlist where_opt upsert",
 /* 170 */ "upsert ::= ON CONFLICT LP sortlist RP where_opt DO NOTHING upsert",
 /* 171 */ "upsert ::= ON CONFLICT DO NOTHING returning",
 /* 172 */ "upsert ::= ON CONFLICT DO UPDATE SET setlist where_opt returning",
 /* 173 */ "returning ::= RETURNING selcollist",
 /* 174 */ "returning ::=",
 /* 175 */ "expr ::= error",
 /* 176 */ "expr ::= term",
 /* 177 */ "expr ::= LP expr RP",
 /* 178 */ "expr ::= expr PLUS|MINUS expr",
 /* 179 */ "expr ::= expr STAR|SLASH|REM expr",
 /* 180 */ "expr ::= expr LT|GT|GE|LE expr",
 /* 181 */ "expr ::= expr EQ|NE expr",
 /* 182 */ "expr ::= expr AND expr",
 /* 183 */ "expr ::= expr OR expr",
 /* 184 */ "expr ::= expr BITAND|BITOR|LSHIFT|RSHIFT expr",
 /* 185 */ "expr ::= expr CONCAT expr",
 /* 186 */ "expr ::= expr PTR expr",
 /* 187 */ "expr ::= PLUS|MINUS expr",
 /* 188 */ "expr ::= BITNOT expr",
 /* 189 */ "expr ::= NOT expr",
 /* 190 */ "exprlist ::= nexprlist",
 /* 191 */ "exprlist ::=",
 /* 192 */ "nexprlist ::= nexprlist COMMA expr",
 /* 193 */ "nexprlist ::= expr",
 /* 194 */ "expr ::= LP nexprlist COMMA expr RP",
 /* 195 */ "expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP",
 /* 196 */ "expr ::= ID|INDEXED|JOIN_KW LP STAR RP",
 /* 197 */ "expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP filter_over",
 /* 198 */ "expr ::= ID|INDEXED|JOIN_KW LP STAR RP filter_over",
 /* 199 */ "nm ::= ID|INDEXED|JOIN_KW",
 /* 200 */ "nm ::= STRING",
 /* 201 */ "nmorerr ::= nm",
 /* 202 */ "nmorerr ::= error",
 /* 203 */ "term ::= INTEGER",
 /* 204 */ "term ::= STRING",
 /* 205 */ "term ::= NULL|FLOAT|BLOB",
 /* 206 */ "term ::= QNUMBER",
 /* 207 */ "term ::= CTIME_KW",
 /* 208 */ "expr ::= VARIABLE",
 /* 209 */ "expr ::= expr COLLATE ID|STRING",
 /* 210 */ "sortlist ::= sortlist COMMA expr sortorder nulls",
 /* 211 */ "sortlist ::= expr sortorder nulls",
 /* 212 */ "sortorder ::= ASC",
 /* 213 */ "sortorder ::= DESC",
 /* 214 */ "sortorder ::=",
 /* 215 */ "nulls ::= NULLS FIRST",
 /* 216 */ "nulls ::= NULLS LAST",
 /* 217 */ "nulls ::=",
 /* 218 */ "expr ::= RAISE LP IGNORE RP",
 /* 219 */ "expr ::= RAISE LP raisetype COMMA expr RP",
 /* 220 */ "raisetype ::= ROLLBACK",
 /* 221 */ "raisetype ::= ABORT",
 /* 222 */ "raisetype ::= FAIL",
 /* 223 */ "fullname ::= nmorerr",
 /* 224 */ "fullname ::= nmorerr DOT nmorerr",
 /* 225 */ "ifexists ::= IF EXISTS",
 /* 226 */ "ifexists ::=",
 /* 227 */ "cmd ::= DROP TABLE ifexists fullname",
 /* 228 */ "cmd ::= DROP VIEW ifexists fullname",
 /* 229 */ "cmd ::= DROP INDEX ifexists fullname",
 /* 230 */ "cmd ::= DROP TRIGGER ifexists fullname",
 /* 231 */ "cmd ::= ALTER TABLE fullname RENAME TO nmorerr",
 /* 232 */ "cmd ::= ALTER TABLE fullname RENAME kwcolumn_opt nmorerr TO nmorerr",
 /* 233 */ "cmd ::= ALTER TABLE fullname DROP kwcolumn_opt nmorerr",
 /* 234 */ "cmd ::= ALTER TABLE add_column_fullname ADD kwcolumn_opt columnname carglist",
 /* 235 */ "add_column_fullname ::= fullname",
 /* 236 */ "kwcolumn_opt ::=",
 /* 237 */ "kwcolumn_opt ::= COLUMNKW",
 /* 238 */ "columnname ::= nmorerr typetoken",
 /* 239 */ "cmd ::= BEGIN transtype trans_opt",
 /* 240 */ "cmd ::= COMMIT|END trans_opt",
 /* 241 */ "cmd ::= ROLLBACK trans_opt",
 /* 242 */ "transtype ::=",
 /* 243 */ "transtype ::= DEFERRED",
 /* 244 */ "transtype ::= IMMEDIATE",
 /* 245 */ "transtype ::= EXCLUSIVE",
 /* 246 */ "trans_opt ::=",
 /* 247 */ "trans_opt ::= TRANSACTION",
 /* 248 */ "trans_opt ::= TRANSACTION nm",
 /* 249 */ "savepoint_opt ::= SAVEPOINT",
 /* 250 */ "savepoint_opt ::=",
 /* 251 */ "cmd ::= SAVEPOINT nmorerr",
 /* 252 */ "cmd ::= RELEASE savepoint_opt nmorerr",
 /* 253 */ "cmd ::= ROLLBACK trans_opt TO savepoint_opt nmorerr",
 /* 254 */ "cmd ::= select",
 /* 255 */ "select ::= selectnowith",
 /* 256 */ "selectnowith ::= oneselect",
 /* 257 */ "oneselect ::= SELECT distinct selcollist from where_opt groupby_opt having_opt orderby_opt limit_opt",
 /* 258 */ "oneselect ::= SELECT distinct selcollist from where_opt groupby_opt having_opt window_clause orderby_opt limit_opt",
 /* 259 */ "selcollist ::= sclp scanpt expr scanpt as",
 /* 260 */ "selcollist ::= sclp scanpt STAR",
 /* 261 */ "sclp ::= selcollist COMMA",
 /* 262 */ "sclp ::=",
 /* 263 */ "scanpt ::=",
 /* 264 */ "as ::= AS nmorerr",
 /* 265 */ "as ::= ID|STRING",
 /* 266 */ "as ::=",
 /* 267 */ "distinct ::= DISTINCT",
 /* 268 */ "distinct ::= ALL",
 /* 269 */ "distinct ::=",
 /* 270 */ "from ::=",
 /* 271 */ "from ::= FROM seltablist",
 /* 272 */ "where_opt ::=",
 /* 273 */ "where_opt ::= WHERE expr",
 /* 274 */ "groupby_opt ::=",
 /* 275 */ "groupby_opt ::= GROUP BY nexprlist",
 /* 276 */ "having_opt ::=",
 /* 277 */ "having_opt ::= HAVING expr",
 /* 278 */ "orderby_opt ::=",
 /* 279 */ "orderby_opt ::= ORDER BY sortlist",
 /* 280 */ "limit_opt ::=",
 /* 281 */ "limit_opt ::= LIMIT expr",
 /* 282 */ "limit_opt ::= LIMIT expr OFFSET expr",
 /* 283 */ "limit_opt ::= LIMIT expr COMMA expr",
 /* 284 */ "stl_prefix ::= seltablist joinop",
 /* 285 */ "stl_prefix ::=",
 /* 286 */ "seltablist ::= stl_prefix nm dbnm as on_using",
 /* 287 */ "seltablist ::= stl_prefix nm dbnm as indexed_by on_using",
 /* 288 */ "seltablist ::= stl_prefix nm dbnm LP exprlist RP as on_using",
 /* 289 */ "seltablist ::= stl_prefix LP select RP as on_using",
 /* 290 */ "seltablist ::= stl_prefix LP seltablist RP as on_using",
 /* 291 */ "joinop ::= COMMA|JOIN",
 /* 292 */ "joinop ::= JOIN_KW JOIN",
 /* 293 */ "joinop ::= JOIN_KW nm JOIN",
 /* 294 */ "joinop ::= JOIN_KW nm nm JOIN",
 /* 295 */ "on_using ::= ON expr",
 /* 296 */ "on_using ::= USING LP idlist RP",
 /* 297 */ "on_using ::=",
 /* 298 */ "indexed_by ::= INDEXED BY nm",
 /* 299 */ "indexed_by ::= NOT INDEXED",
 /* 300 */ "idlist ::= idlist COMMA nm",
 /* 301 */ "idlist ::= nm",
 /* 302 */ "cmd ::= createkw trigger_decl BEGIN trigger_cmd_list END",
 /* 303 */ "trigger_decl ::= temp TRIGGER ifnotexists nm dbnm trigger_time trigger_event ON fullname foreach_clause when_clause",
 /* 304 */ "trigger_time ::= BEFORE|AFTER",
 /* 305 */ "trigger_time ::= INSTEAD OF",
 /* 306 */ "trigger_time ::=",
 /* 307 */ "trigger_event ::= DELETE|INSERT",
 /* 308 */ "trigger_event ::= UPDATE",
 /* 309 */ "trigger_event ::= UPDATE OF idlist",
 /* 310 */ "foreach_clause ::=",
 /* 311 */ "foreach_clause ::= FOR EACH ROW",
 /* 312 */ "when_clause ::=",
 /* 313 */ "when_clause ::= WHEN expr",
 /* 314 */ "trigger_cmd_list ::= trigger_cmd_list trigger_cmd SEMI",
 /* 315 */ "trigger_cmd_list ::= trigger_cmd SEMI",
 /* 316 */ "trnm ::= nm",
 /* 317 */ "trnm ::= nm DOT nm",
 /* 318 */ "tridxby ::=",
 /* 319 */ "tridxby ::= INDEXED BY nm",
 /* 320 */ "tridxby ::= NOT INDEXED",
 /* 321 */ "trigger_cmd ::= UPDATE orconf trnm tridxby SET setlist from where_opt scanpt",
 /* 322 */ "trigger_cmd ::= scanpt insert_cmd INTO trnm idlist_opt select upsert scanpt",
 /* 323 */ "trigger_cmd ::= DELETE FROM trnm tridxby where_opt scanpt",
 /* 324 */ "trigger_cmd ::= scanpt select scanpt",
 /* 325 */ "cmd ::= PRAGMA nm dbnm",
 /* 326 */ "cmd ::= PRAGMA nm dbnm EQ nmnum",
 /* 327 */ "cmd ::= PRAGMA nm dbnm LP nmnum RP",
 /* 328 */ "cmd ::= PRAGMA nm dbnm EQ minus_num",
 /* 329 */ "cmd ::= PRAGMA nm dbnm LP minus_num RP",
 /* 330 */ "nmnum ::= plus_num",
 /* 331 */ "nmnum ::= nm",
 /* 332 */ "nmnum ::= ON",
 /* 333 */ "nmnum ::= DELETE",
 /* 334 */ "nmnum ::= DEFAULT",
 /* 335 */ "plus_num ::= PLUS INTEGER|FLOAT",
 /* 336 */ "plus_num ::= INTEGER|FLOAT",
 /* 337 */ "minus_num ::= MINUS INTEGER|FLOAT",
 /* 338 */ "signed ::= plus_num",
 /* 339 */ "signed ::= minus_num",
 /* 340 */ "cmd ::= ANALYZE",
 /* 341 */ "cmd ::= ANALYZE nm dbnm",
 /* 342 */ "cmd ::= REINDEX",
 /* 343 */ "cmd ::= REINDEX nm dbnm",
 /* 344 */ "cmd ::= ATTACH database_kw_opt expr AS expr key_opt",
 /* 345 */ "cmd ::= DETACH database_kw_opt expr",
 /* 346 */ "database_kw_opt ::= DATABASE",
 /* 347 */ "database_kw_opt ::=",
 /* 348 */ "key_opt ::=",
 /* 349 */ "key_opt ::= KEY expr",
 /* 350 */ "cmd ::= VACUUM vinto",
 /* 351 */ "cmd ::= VACUUM nm vinto",
 /* 352 */ "vinto ::= INTO expr",
 /* 353 */ "vinto ::=",
 /* 354 */ "ecmd ::= explain cmdx SEMI",
 /* 355 */ "explain ::= EXPLAIN",
 /* 356 */ "explain ::= EXPLAIN QUERY PLAN",
 /* 357 */ "cmd ::= createkw uniqueflag INDEX ifnotexists nm dbnm ON nm LP sortlist RP where_opt",
 /* 358 */ "uniqueflag ::= UNIQUE",
 /* 359 */ "uniqueflag ::=",
 /* 360 */ "ifnotexists ::=",
 /* 361 */ "ifnotexists ::= IF NOT EXISTS",
 /* 362 */ "cmd ::= createkw temp VIEW ifnotexists nm dbnm eidlist_opt AS select",
 /* 363 */ "createkw ::= CREATE",
 /* 364 */ "temp ::= TEMP",
 /* 365 */ "temp ::=",
 /* 366 */ "values ::= VALUES LP nexprlist RP",
 /* 367 */ "mvalues ::= values COMMA LP nexprlist RP",
 /* 368 */ "mvalues ::= mvalues COMMA LP nexprlist RP",
 /* 369 */ "oneselect ::= values",
 /* 370 */ "oneselect ::= mvalues",
 /* 371 */ "cmd ::= create_vtab",
 /* 372 */ "cmd ::= create_vtab LP vtabarglist RP",
 /* 373 */ "create_vtab ::= createkw VIRTUAL TABLE ifnotexists nm dbnm USING nm",
 /* 374 */ "vtabarglist ::= vtabarg",
 /* 375 */ "vtabarglist ::= vtabarglist COMMA vtabarg",
 /* 376 */ "vtabarg ::=",
 /* 377 */ "vtabarg ::= vtabarg vtabargtoken",
 /* 378 */ "vtabargtoken ::= ANY",
 /* 379 */ "vtabargtoken ::= lp anylist RP",
 /* 380 */ "lp ::= LP",
 /* 381 */ "anylist ::=",
 /* 382 */ "anylist ::= anylist LP anylist RP",
 /* 383 */ "anylist ::= anylist ANY",
 /* 384 */ "windowdefn_list ::= windowdefn",
 /* 385 */ "windowdefn_list ::= windowdefn_list COMMA windowdefn",
 /* 386 */ "windowdefn ::= nm AS LP window RP",
 /* 387 */ "window ::= PARTITION BY nexprlist orderby_opt frame_opt",
 /* 388 */ "window ::= nm PARTITION BY nexprlist orderby_opt frame_opt",
 /* 389 */ "window ::= ORDER BY sortlist frame_opt",
 /* 390 */ "window ::= nm ORDER BY sortlist frame_opt",
 /* 391 */ "window ::= frame_opt",
 /* 392 */ "window ::= nm frame_opt",
 /* 393 */ "frame_opt ::=",
 /* 394 */ "frame_opt ::= range_or_rows frame_bound_s frame_exclude_opt",
 /* 395 */ "frame_opt ::= range_or_rows BETWEEN frame_bound_s AND frame_bound_e frame_exclude_opt",
 /* 396 */ "range_or_rows ::= RANGE|ROWS|GROUPS",
 /* 397 */ "frame_bound_s ::= frame_bound",
 /* 398 */ "frame_bound_s ::= UNBOUNDED PRECEDING",
 /* 399 */ "frame_bound_e ::= frame_bound",
 /* 400 */ "frame_bound_e ::= UNBOUNDED FOLLOWING",
 /* 401 */ "frame_bound ::= expr PRECEDING|FOLLOWING",
 /* 402 */ "frame_bound ::= CURRENT ROW",
 /* 403 */ "frame_exclude_opt ::=",
 /* 404 */ "frame_exclude_opt ::= EXCLUDE frame_exclude",
 /* 405 */ "frame_exclude ::= NO OTHERS",
 /* 406 */ "frame_exclude ::= CURRENT ROW",
 /* 407 */ "frame_exclude ::= GROUP|TIES",
 /* 408 */ "window_clause ::= WINDOW windowdefn_list",
 /* 409 */ "filter_over ::= filter_clause over_clause",
 /* 410 */ "filter_over ::= over_clause",
 /* 411 */ "filter_over ::= filter_clause",
 /* 412 */ "over_clause ::= OVER LP window RP",
 /* 413 */ "over_clause ::= OVER nm",
 /* 414 */ "filter_clause ::= FILTER LP WHERE expr RP",
 /* 415 */ "perfetto_or_replace ::=",
 /* 416 */ "perfetto_or_replace ::= OR REPLACE",
 /* 417 */ "perfetto_arg_type ::= ID",
 /* 418 */ "perfetto_arg_type ::= ID LP ID DOT ID RP",
 /* 419 */ "perfetto_arg_def_list ::=",
 /* 420 */ "perfetto_arg_def_list ::= perfetto_arg_def_list_ne",
 /* 421 */ "perfetto_arg_def_list_ne ::= ID perfetto_arg_type",
 /* 422 */ "perfetto_arg_def_list_ne ::= perfetto_arg_def_list_ne COMMA ID perfetto_arg_type",
 /* 423 */ "perfetto_arg_def_list_ne ::= ID perfetto_arg_type DOT DOT DOT",
 /* 424 */ "perfetto_arg_def_list_ne ::= perfetto_arg_def_list_ne COMMA ID perfetto_arg_type DOT DOT DOT",
 /* 425 */ "perfetto_table_schema ::=",
 /* 426 */ "perfetto_table_schema ::= LP perfetto_arg_def_list_ne RP",
 /* 427 */ "perfetto_table_impl ::=",
 /* 428 */ "perfetto_table_impl ::= USING ID",
 /* 429 */ "perfetto_return_type ::= ID",
 /* 430 */ "perfetto_return_type ::= TABLE LP perfetto_arg_def_list_ne RP",
 /* 431 */ "perfetto_indexed_col_list ::= ID",
 /* 432 */ "perfetto_indexed_col_list ::= perfetto_indexed_col_list COMMA ID",
 /* 433 */ "perfetto_macro_arg_list ::=",
 /* 434 */ "perfetto_macro_arg_list ::= perfetto_macro_arg_list_ne",
 /* 435 */ "perfetto_macro_arg_list_ne ::= ID ID",
 /* 436 */ "perfetto_macro_arg_list_ne ::= perfetto_macro_arg_list_ne COMMA ID ID",
 /* 437 */ "perfetto_module_name ::= ID|STAR|INTERSECT",
 /* 438 */ "perfetto_module_name ::= perfetto_module_name DOT ID|STAR|INTERSECT",
 /* 439 */ "select_body_start ::=",
 /* 440 */ "select_body_end ::=",
 /* 441 */ "perfetto_pipe ::= BITOR GT",
 /* 442 */ "perfetto_pipe_source ::= nm dbnm as",
 /* 443 */ "perfetto_pipe_source ::= LP select RP as",
 /* 444 */ "perfetto_tree_direction ::= UP",
 /* 445 */ "perfetto_tree_direction ::= DOWN",
 /* 446 */ "perfetto_tree_aggregate ::= expr AS nm",
 /* 447 */ "perfetto_tree_aggregate_list ::= perfetto_tree_aggregate",
 /* 448 */ "perfetto_tree_aggregate_list ::= perfetto_tree_aggregate_list COMMA perfetto_tree_aggregate",
 /* 449 */ "perfetto_pipe_select_item ::= nm",
 /* 450 */ "perfetto_pipe_select_item ::= nm AS nm",
 /* 451 */ "perfetto_pipe_select_item ::= nm DOT nm",
 /* 452 */ "perfetto_pipe_select_item ::= nm DOT nm AS nm",
 /* 453 */ "perfetto_pipe_select_list ::= perfetto_pipe_select_item",
 /* 454 */ "perfetto_pipe_select_list ::= perfetto_pipe_select_list COMMA perfetto_pipe_select_item",
 /* 455 */ "perfetto_pipe_stage ::= SELECT perfetto_pipe_select_list",
 /* 456 */ "perfetto_pipe_stage ::= TREE ACCUMULATE perfetto_tree_direction perfetto_tree_aggregate_list",
 /* 457 */ "perfetto_pipe_stage_list ::=",
 /* 458 */ "perfetto_pipe_stage_list ::= perfetto_pipe_stage_list perfetto_pipe perfetto_pipe_stage",
 /* 459 */ "perfetto_pipe_source_list ::= perfetto_pipe_source",
 /* 460 */ "perfetto_pipe_source_list ::= perfetto_pipe_source_list COMMA perfetto_pipe_source",
 /* 461 */ "perfetto_per_col_list ::= nm",
 /* 462 */ "perfetto_per_col_list ::= perfetto_per_col_list COMMA nm",
 /* 463 */ "perfetto_per ::=",
 /* 464 */ "perfetto_per ::= PER perfetto_per_col_list",
 /* 465 */ "perfetto_pipeline ::= FROM perfetto_pipe_source perfetto_pipe_stage_list",
 /* 466 */ "perfetto_pipeline ::= INTERVAL INTERSECTION OF LP perfetto_pipe_source_list RP perfetto_per perfetto_pipe_stage_list",
 /* 467 */ "cmd ::= perfetto_pipeline",
 /* 468 */ "cmd ::= PERFETTO PRAGMA nm EQ expr",
 /* 469 */ "cmd ::= CREATE perfetto_or_replace PERFETTO TABLE nm perfetto_table_impl perfetto_table_schema AS select_body_start select select_body_end",
 /* 470 */ "cmd ::= CREATE perfetto_or_replace PERFETTO TABLE nm perfetto_table_impl perfetto_table_schema AS perfetto_pipeline",
 /* 471 */ "cmd ::= CREATE perfetto_or_replace PERFETTO VIEW nm perfetto_table_schema AS select_body_start select select_body_end",
 /* 472 */ "cmd ::= CREATE perfetto_or_replace PERFETTO FUNCTION nm LP perfetto_arg_def_list RP RETURNS perfetto_return_type AS select_body_start select select_body_end",
 /* 473 */ "cmd ::= CREATE perfetto_or_replace PERFETTO FUNCTION nm LP perfetto_arg_def_list RP RETURNS perfetto_return_type DELEGATES TO ID",
 /* 474 */ "cmd ::= CREATE perfetto_or_replace PERFETTO INDEX nm ON nm LP perfetto_indexed_col_list RP",
 /* 475 */ "before_macro_body ::=",
 /* 476 */ "perfetto_macro_body ::= ANY",
 /* 477 */ "perfetto_macro_body ::= perfetto_macro_body ANY",
 /* 478 */ "cmd ::= CREATE perfetto_or_replace PERFETTO MACRO nm LP perfetto_macro_arg_list RP RETURNS ID before_macro_body AS perfetto_macro_body",
 /* 479 */ "cmd ::= INCLUDE PERFETTO MODULE perfetto_module_name",
 /* 480 */ "cmd ::= DROP PERFETTO INDEX nm ON nm",
};
#endif /* NDEBUG */


#if YYGROWABLESTACK
/*
** Try to increase the size of the parser stack.  Return the number
** of errors.  Return 0 on success.
*/
static int yyGrowStack(yyParser *p){
  int oldSize = 1 + (int)(p->yystackEnd - p->yystack);
  int newSize;
  int idx;
  yyStackEntry *pNew;

  newSize = oldSize*2 + 100;
  idx = (int)(p->yytos - p->yystack);
  if( p->yystack==p->yystk0 ){
    pNew = YYREALLOC(0, newSize*sizeof(pNew[0]));
    if( pNew==0 ) return 1;
    memcpy(pNew, p->yystack, oldSize*sizeof(pNew[0]));
  }else{
    pNew = YYREALLOC(p->yystack, newSize*sizeof(pNew[0]));
    if( pNew==0 ) return 1;
  }
  p->yystack = pNew;
  p->yytos = &p->yystack[idx];
#ifndef NDEBUG
  if( yyTraceFILE ){
    fprintf(yyTraceFILE,"%sStack grows from %d to %d entries.\n",
            yyTracePrompt, oldSize, newSize);
  }
#endif
  p->yystackEnd = &p->yystack[newSize-1];
  return 0;
}
#endif /* YYGROWABLESTACK */

#if !YYGROWABLESTACK
/* For builds that do no have a growable stack, yyGrowStack always
** returns an error.
*/
# define yyGrowStack(X) 1
#endif

/* Datatype of the argument to the memory allocated passed as the
** second argument to SynqPerfettoParseAlloc() below.  This can be changed by
** putting an appropriate #define in the %include section of the input
** grammar.
*/
#ifndef YYMALLOCARGTYPE
# define YYMALLOCARGTYPE size_t
#endif

/* Initialize a new parser that has already been allocated.
*/
void SynqPerfettoParseInit(void *yypRawParser SynqPerfettoParseCTX_PDECL){
  yyParser *yypParser = (yyParser*)yypRawParser;
  SynqPerfettoParseCTX_STORE
#ifdef YYTRACKMAXSTACKDEPTH
  yypParser->yyhwm = 0;
#endif
  yypParser->yystack = yypParser->yystk0;
  yypParser->yystackEnd = &yypParser->yystack[YYSTACKDEPTH-1];
#ifndef YYNOERRORRECOVERY
  yypParser->yyerrcnt = -1;
#endif
  yypParser->yytos = yypParser->yystack;
  yypParser->yystack[0].stateno = 0;
  yypParser->yystack[0].major = 0;
}

#ifndef SynqPerfettoParse_ENGINEALWAYSONSTACK
/* 
** This function allocates a new parser.
** The only argument is a pointer to a function which works like
** malloc.
**
** Inputs:
** A pointer to the function used to allocate memory.
**
** Outputs:
** A pointer to a parser.  This pointer is used in subsequent calls
** to SynqPerfettoParse and SynqPerfettoParseFree.
*/
void *SynqPerfettoParseAlloc(void *(*mallocProc)(YYMALLOCARGTYPE) SynqPerfettoParseCTX_PDECL){
  yyParser *yypParser;
  yypParser = (yyParser*)(*mallocProc)( (YYMALLOCARGTYPE)sizeof(yyParser) );
  if( yypParser ){
    SynqPerfettoParseCTX_STORE
    SynqPerfettoParseInit(yypParser SynqPerfettoParseCTX_PARAM);
  }
  return (void*)yypParser;
}
#endif /* SynqPerfettoParse_ENGINEALWAYSONSTACK */


/* The following function deletes the "minor type" or semantic value
** associated with a symbol.  The symbol can be either a terminal
** or nonterminal. "yymajor" is the symbol code, and "yypminor" is
** a pointer to the value to be deleted.  The code used to do the 
** deletions is derived from the %destructor and/or %token_destructor
** directives of the input grammar.
*/
static void yy_destructor(
  yyParser *yypParser,    /* The parser */
  YYCODETYPE yymajor,     /* Type code for object to destroy */
  YYMINORTYPE *yypminor   /* The object to be destroyed */
){
  SynqPerfettoParseARG_FETCH
  SynqPerfettoParseCTX_FETCH
  switch( yymajor ){
    /* Here is inserted the actions which take place when a
    ** terminal or non-terminal is destroyed.  This can happen
    ** when the symbol is popped from the stack during a
    ** reduce or during error processing or when a parser is 
    ** being destroyed before it is finished parsing.
    **
    ** Note: during a reduce, the only symbols destroyed are those
    ** which appear on the RHS of the rule, but which are *not* used
    ** inside the C code.
    */
/********* Begin destructor definitions ***************************************/
/********* End destructor definitions *****************************************/
    default:  break;   /* If no destructor action specified: do nothing */
  }
}

/*
** Pop the parser's stack once.
**
** If there is a destructor routine associated with the token which
** is popped from the stack, then call it.
*/
static void yy_pop_parser_stack(yyParser *pParser){
  yyStackEntry *yytos;
  assert( pParser->yytos!=0 );
  assert( pParser->yytos > pParser->yystack );
  yytos = pParser->yytos--;
#ifndef NDEBUG
  if( yyTraceFILE ){
    fprintf(yyTraceFILE,"%sPopping %s\n",
      yyTracePrompt,
      yyTokenName[yytos->major]);
  }
#endif
  yy_destructor(pParser, yytos->major, &yytos->minor);
}

/*
** Clear all secondary memory allocations from the parser
*/
void SynqPerfettoParseFinalize(void *p){
  yyParser *pParser = (yyParser*)p;

  /* In-lined version of calling yy_pop_parser_stack() for each
  ** element left in the stack */
  yyStackEntry *yytos = pParser->yytos;
  while( yytos>pParser->yystack ){
#ifndef NDEBUG
    if( yyTraceFILE ){
      fprintf(yyTraceFILE,"%sPopping %s\n",
        yyTracePrompt,
        yyTokenName[yytos->major]);
    }
#endif
    if( yytos->major>=YY_MIN_DSTRCTR ){
      yy_destructor(pParser, yytos->major, &yytos->minor);
    }
    yytos--;
  }

#if YYGROWABLESTACK
  if( pParser->yystack!=pParser->yystk0 ) YYFREE(pParser->yystack);
#endif
}

#ifndef SynqPerfettoParse_ENGINEALWAYSONSTACK
/* 
** Deallocate and destroy a parser.  Destructors are called for
** all stack elements before shutting the parser down.
**
** If the YYPARSEFREENEVERNULL macro exists (for example because it
** is defined in a %include section of the input grammar) then it is
** assumed that the input pointer is never NULL.
*/
void SynqPerfettoParseFree(
  void *p,                    /* The parser to be deleted */
  void (*freeProc)(void*)     /* Function used to reclaim memory */
){
#ifndef YYPARSEFREENEVERNULL
  if( p==0 ) return;
#endif
  SynqPerfettoParseFinalize(p);
  (*freeProc)(p);
}
#endif /* SynqPerfettoParse_ENGINEALWAYSONSTACK */

/*
** Return the peak depth of the stack for a parser.
*/
#ifdef YYTRACKMAXSTACKDEPTH
int SynqPerfettoParseStackPeak(void *p){
  yyParser *pParser = (yyParser*)p;
  return pParser->yyhwm;
}
#endif

/* This array of booleans keeps track of the parser statement
** coverage.  The element yycoverage[X][Y] is set when the parser
** is in state X and has a lookahead token Y.  In a well-tested
** systems, every element of this matrix should end up being set.
*/
#if defined(YYCOVERAGE)
static unsigned char yycoverage[YYNSTATE][YYNTOKEN];
#endif

/*
** Write into out a description of every state/lookahead combination that
**
**   (1)  has not been used by the parser, and
**   (2)  is not a syntax error.
**
** Return the number of missed state/lookahead combinations.
*/
#if defined(YYCOVERAGE)
int SynqPerfettoParseCoverage(FILE *out){
  int stateno, iLookAhead, i;
  int nMissed = 0;
  for(stateno=0; stateno<YYNSTATE; stateno++){
    i = yy_shift_ofst[stateno];
    for(iLookAhead=0; iLookAhead<YYNTOKEN; iLookAhead++){
      if( yy_lookahead[i+iLookAhead]!=iLookAhead ) continue;
      if( yycoverage[stateno][iLookAhead]==0 ) nMissed++;
      if( out ){
        fprintf(out,"State %d lookahead %s %s\n", stateno,
                yyTokenName[iLookAhead],
                yycoverage[stateno][iLookAhead] ? "ok" : "missed");
      }
    }
  }
  return nMissed;
}
#endif

/*
** Find the appropriate action for a parser given the terminal
** look-ahead token iLookAhead.
*/
static YYACTIONTYPE yy_find_shift_action(
  YYCODETYPE iLookAhead,    /* The look-ahead token */
  YYACTIONTYPE stateno      /* Current state number */
){
  int i;

  if( stateno>YY_MAX_SHIFT ) return stateno;
  assert( stateno <= YY_SHIFT_COUNT );
#if defined(YYCOVERAGE)
  yycoverage[stateno][iLookAhead] = 1;
#endif
  do{
    i = yy_shift_ofst[stateno];
    assert( i>=0 );
    assert( i<=YY_ACTTAB_COUNT );
    assert( i+YYNTOKEN<=(int)YY_NLOOKAHEAD );
    assert( iLookAhead!=YYNOCODE );
    assert( iLookAhead < YYNTOKEN );
    i += iLookAhead;
    assert( i<(int)YY_NLOOKAHEAD );
    if( yy_lookahead[i]!=iLookAhead ){
#ifdef YYFALLBACK
      YYCODETYPE iFallback;            /* Fallback token */
      assert( iLookAhead<sizeof(yyFallback)/sizeof(yyFallback[0]) );
      iFallback = yyFallback[iLookAhead];
      if( iFallback!=0 ){
#ifndef NDEBUG
        if( yyTraceFILE ){
          fprintf(yyTraceFILE, "%sFALLBACK %s => %s\n",
             yyTracePrompt, yyTokenName[iLookAhead], yyTokenName[iFallback]);
        }
#endif
        assert( yyFallback[iFallback]==0 ); /* Fallback loop must terminate */
        iLookAhead = iFallback;
        continue;
      }
#endif
#ifdef YYWILDCARD
      {
        int j = i - iLookAhead + YYWILDCARD;
        assert( j<(int)(sizeof(yy_lookahead)/sizeof(yy_lookahead[0])) );
        if( yy_lookahead[j]==YYWILDCARD && iLookAhead>0 ){
#ifndef NDEBUG
          if( yyTraceFILE ){
            fprintf(yyTraceFILE, "%sWILDCARD %s => %s\n",
               yyTracePrompt, yyTokenName[iLookAhead],
               yyTokenName[YYWILDCARD]);
          }
#endif /* NDEBUG */
          return yy_action[j];
        }
      }
#endif /* YYWILDCARD */
      return yy_default[stateno];
    }else{
      assert( i>=0 && i<(int)(sizeof(yy_action)/sizeof(yy_action[0])) );
      return yy_action[i];
    }
  }while(1);
}

/*
** Find the appropriate action for a parser given the non-terminal
** look-ahead token iLookAhead.
*/
static YYACTIONTYPE yy_find_reduce_action(
  YYACTIONTYPE stateno,     /* Current state number */
  YYCODETYPE iLookAhead     /* The look-ahead token */
){
  int i;
#ifdef YYERRORSYMBOL
  if( stateno>YY_REDUCE_COUNT ){
    return yy_default[stateno];
  }
#else
  assert( stateno<=YY_REDUCE_COUNT );
#endif
  i = yy_reduce_ofst[stateno];
  assert( iLookAhead!=YYNOCODE );
  i += iLookAhead;
#ifdef YYERRORSYMBOL
  if( i<0 || i>=YY_ACTTAB_COUNT || yy_lookahead[i]!=iLookAhead ){
    return yy_default[stateno];
  }
#else
  assert( i>=0 && i<YY_ACTTAB_COUNT );
  assert( yy_lookahead[i]==iLookAhead );
#endif
  return yy_action[i];
}

/*
** The following routine is called if the stack overflows.
*/
static void yyStackOverflow(yyParser *yypParser){
   SynqPerfettoParseARG_FETCH
   SynqPerfettoParseCTX_FETCH
#ifndef NDEBUG
   if( yyTraceFILE ){
     fprintf(yyTraceFILE,"%sStack Overflow!\n",yyTracePrompt);
   }
#endif
   while( yypParser->yytos>yypParser->yystack ) yy_pop_parser_stack(yypParser);
   /* Here code is inserted which will execute if the parser
   ** stack every overflows */
/******** Begin %stack_overflow code ******************************************/

  if (pCtx) {
    pCtx->error = 1;
  }
/******** End %stack_overflow code ********************************************/
   SynqPerfettoParseARG_STORE /* Suppress warning about unused %extra_argument var */
   SynqPerfettoParseCTX_STORE
}

/*
** Print tracing information for a SHIFT action
*/
#ifndef NDEBUG
static void yyTraceShift(yyParser *yypParser, int yyNewState, const char *zTag){
  if( yyTraceFILE ){
    if( yyNewState<YYNSTATE ){
      fprintf(yyTraceFILE,"%s%s '%s', go to state %d\n",
         yyTracePrompt, zTag, yyTokenName[yypParser->yytos->major],
         yyNewState);
    }else{
      fprintf(yyTraceFILE,"%s%s '%s', pending reduce %d\n",
         yyTracePrompt, zTag, yyTokenName[yypParser->yytos->major],
         yyNewState - YY_MIN_REDUCE);
    }
  }
}
#else
# define yyTraceShift(X,Y,Z)
#endif

/*
** Perform a shift action.
*/
static void yy_shift(
  yyParser *yypParser,          /* The parser to be shifted */
  YYACTIONTYPE yyNewState,      /* The new state to shift in */
  YYCODETYPE yyMajor,           /* The major token to shift in */
  SynqPerfettoParseTOKENTYPE yyMinor        /* The minor token to shift in */
){
  yyStackEntry *yytos;
  yypParser->yytos++;
#ifdef YYTRACKMAXSTACKDEPTH
  if( (int)(yypParser->yytos - yypParser->yystack)>yypParser->yyhwm ){
    yypParser->yyhwm++;
    assert( yypParser->yyhwm == (int)(yypParser->yytos - yypParser->yystack) );
  }
#endif
  yytos = yypParser->yytos;
  if( yytos>yypParser->yystackEnd ){
    if( yyGrowStack(yypParser) ){
      yypParser->yytos--;
      yyStackOverflow(yypParser);
      return;
    }
    yytos = yypParser->yytos;
    assert( yytos <= yypParser->yystackEnd );
  }
  if( yyNewState > YY_MAX_SHIFT ){
    yyNewState += YY_MIN_REDUCE - YY_MIN_SHIFTREDUCE;
  }
  yytos->stateno = yyNewState;
  yytos->major = yyMajor;
  yytos->minor.yy0 = yyMinor;
  synq_on_shift(yypParser, yyMajor, &yyMinor);
  yyTraceShift(yypParser, yyNewState, "Shift");
}

/* For rule J, yyRuleInfoLhs[J] contains the symbol on the left-hand side
** of that rule */
static const YYCODETYPE yyRuleInfoLhs[] = {
   202,  /* (0) input ::= cmdlist */
   203,  /* (1) cmdlist ::= cmdlist ecmd */
   203,  /* (2) cmdlist ::= ecmd */
   204,  /* (3) ecmd ::= SEMI */
   204,  /* (4) ecmd ::= cmdx SEMI */
   204,  /* (5) ecmd ::= error SEMI */
   205,  /* (6) cmdx ::= cmd */
   208,  /* (7) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist ORDER BY sortlist RP */
   208,  /* (8) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist ORDER BY sortlist RP filter_over */
   208,  /* (9) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP WITHIN GROUP LP ORDER BY expr RP */
   208,  /* (10) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP WITHIN GROUP LP ORDER BY expr RP filter_over */
   208,  /* (11) expr ::= CAST LP expr AS typetoken RP */
   213,  /* (12) typetoken ::= */
   213,  /* (13) typetoken ::= typename */
   213,  /* (14) typetoken ::= typename LP signed RP */
   213,  /* (15) typetoken ::= typename LP signed COMMA signed RP */
   214,  /* (16) typename ::= ID|STRING */
   214,  /* (17) typename ::= typename ID|STRING */
   216,  /* (18) selcollist ::= sclp scanpt nm DOT STAR */
   208,  /* (19) expr ::= ID|INDEXED|JOIN_KW */
   208,  /* (20) expr ::= nm DOT nm */
   208,  /* (21) expr ::= nm DOT nm DOT nm */
   223,  /* (22) selectnowith ::= selectnowith multiselect_op oneselect */
   220,  /* (23) multiselect_op ::= UNION */
   220,  /* (24) multiselect_op ::= UNION ALL */
   220,  /* (25) multiselect_op ::= EXCEPT|INTERSECT */
   208,  /* (26) expr ::= LP select RP */
   208,  /* (27) expr ::= EXISTS LP select RP */
   221,  /* (28) in_op ::= IN */
   221,  /* (29) in_op ::= NOT IN */
   208,  /* (30) expr ::= expr in_op LP exprlist RP */
   208,  /* (31) expr ::= expr in_op LP select RP */
   208,  /* (32) expr ::= expr in_op nm dbnm paren_exprlist */
   222,  /* (33) dbnm ::= */
   222,  /* (34) dbnm ::= DOT nm */
   226,  /* (35) paren_exprlist ::= */
   226,  /* (36) paren_exprlist ::= LP exprlist RP */
   208,  /* (37) expr ::= expr ISNULL|NOTNULL */
   208,  /* (38) expr ::= expr NOT NULL */
   208,  /* (39) expr ::= expr IS expr */
   208,  /* (40) expr ::= expr IS NOT expr */
   208,  /* (41) expr ::= expr IS NOT DISTINCT FROM expr */
   208,  /* (42) expr ::= expr IS DISTINCT FROM expr */
   228,  /* (43) between_op ::= BETWEEN */
   228,  /* (44) between_op ::= NOT BETWEEN */
   208,  /* (45) expr ::= expr between_op expr AND expr */
   227,  /* (46) likeop ::= LIKE_KW|MATCH */
   227,  /* (47) likeop ::= NOT LIKE_KW|MATCH */
   208,  /* (48) expr ::= expr likeop expr */
   208,  /* (49) expr ::= expr likeop expr ESCAPE expr */
   208,  /* (50) expr ::= CASE case_operand case_exprlist case_else END */
   230,  /* (51) case_exprlist ::= case_exprlist WHEN expr THEN expr */
   230,  /* (52) case_exprlist ::= WHEN expr THEN expr */
   231,  /* (53) case_else ::= ELSE expr */
   231,  /* (54) case_else ::= */
   229,  /* (55) case_operand ::= expr */
   229,  /* (56) case_operand ::= */
   207,  /* (57) cmd ::= create_table create_table_args */
   249,  /* (58) create_table ::= createkw temp TABLE ifnotexists nm dbnm */
   250,  /* (59) create_table_args ::= LP columnlist conslist_opt RP table_option_set */
   250,  /* (60) create_table_args ::= AS select */
   240,  /* (61) table_option_set ::= */
   240,  /* (62) table_option_set ::= table_option */
   240,  /* (63) table_option_set ::= table_option_set COMMA table_option */
   241,  /* (64) table_option ::= WITHOUT nm */
   241,  /* (65) table_option ::= nm */
   254,  /* (66) columnlist ::= columnlist COMMA columnname carglist */
   254,  /* (67) columnlist ::= columnname carglist */
   244,  /* (68) carglist ::= carglist ccons */
   244,  /* (69) carglist ::= */
   243,  /* (70) ccons ::= CONSTRAINT nm */
   243,  /* (71) ccons ::= DEFAULT scantok term */
   243,  /* (72) ccons ::= DEFAULT LP expr RP */
   243,  /* (73) ccons ::= DEFAULT PLUS scantok term */
   243,  /* (74) ccons ::= DEFAULT MINUS scantok term */
   243,  /* (75) ccons ::= DEFAULT scantok ID|INDEXED */
   243,  /* (76) ccons ::= NULL onconf */
   243,  /* (77) ccons ::= NOT NULL onconf */
   243,  /* (78) ccons ::= PRIMARY KEY sortorder onconf autoinc */
   243,  /* (79) ccons ::= UNIQUE onconf */
   243,  /* (80) ccons ::= CHECK LP expr RP */
   243,  /* (81) ccons ::= REFERENCES nm eidlist_opt refargs */
   243,  /* (82) ccons ::= defer_subclause */
   243,  /* (83) ccons ::= COLLATE ID|STRING */
   243,  /* (84) ccons ::= GENERATED ALWAYS AS generated */
   243,  /* (85) ccons ::= AS generated */
   248,  /* (86) generated ::= LP expr RP */
   248,  /* (87) generated ::= LP expr RP ID */
   233,  /* (88) autoinc ::= */
   233,  /* (89) autoinc ::= AUTOINCR */
   234,  /* (90) refargs ::= */
   234,  /* (91) refargs ::= refargs refarg */
   235,  /* (92) refarg ::= MATCH nm */
   235,  /* (93) refarg ::= ON INSERT refact */
   235,  /* (94) refarg ::= ON DELETE refact */
   235,  /* (95) refarg ::= ON UPDATE refact */
   236,  /* (96) refact ::= SET NULL */
   236,  /* (97) refact ::= SET DEFAULT */
   236,  /* (98) refact ::= CASCADE */
   236,  /* (99) refact ::= RESTRICT */
   236,  /* (100) refact ::= NO ACTION */
   237,  /* (101) defer_subclause ::= NOT DEFERRABLE init_deferred_pred_opt */
   237,  /* (102) defer_subclause ::= DEFERRABLE init_deferred_pred_opt */
   238,  /* (103) init_deferred_pred_opt ::= */
   238,  /* (104) init_deferred_pred_opt ::= INITIALLY DEFERRED */
   238,  /* (105) init_deferred_pred_opt ::= INITIALLY IMMEDIATE */
   255,  /* (106) conslist_opt ::= */
   255,  /* (107) conslist_opt ::= COMMA conslist */
   246,  /* (108) conslist ::= conslist tconscomma tcons */
   246,  /* (109) conslist ::= tcons */
   247,  /* (110) tconscomma ::= COMMA */
   247,  /* (111) tconscomma ::= */
   245,  /* (112) tcons ::= CONSTRAINT nm */
   245,  /* (113) tcons ::= PRIMARY KEY LP sortlist autoinc RP onconf */
   245,  /* (114) tcons ::= UNIQUE LP sortlist RP onconf */
   245,  /* (115) tcons ::= CHECK LP expr RP onconf */
   245,  /* (116) tcons ::= FOREIGN KEY LP eidlist RP REFERENCES nm eidlist_opt refargs defer_subclause_opt */
   239,  /* (117) defer_subclause_opt ::= */
   239,  /* (118) defer_subclause_opt ::= defer_subclause */
   242,  /* (119) onconf ::= */
   242,  /* (120) onconf ::= ON CONFLICT resolvetype */
   232,  /* (121) scantok ::= */
   225,  /* (122) select ::= WITH wqlist selectnowith */
   225,  /* (123) select ::= WITH RECURSIVE wqlist selectnowith */
   266,  /* (124) wqitem ::= withnm eidlist_opt wqas LP select RP */
   265,  /* (125) wqlist ::= wqitem */
   265,  /* (126) wqlist ::= wqlist COMMA wqitem */
   262,  /* (127) withnm ::= nm */
   263,  /* (128) wqas ::= AS */
   263,  /* (129) wqas ::= AS MATERIALIZED */
   263,  /* (130) wqas ::= AS NOT MATERIALIZED */
   259,  /* (131) eidlist_opt ::= */
   259,  /* (132) eidlist_opt ::= LP eidlist RP */
   260,  /* (133) eidlist ::= nm collate sortorder */
   260,  /* (134) eidlist ::= eidlist COMMA nm collate sortorder */
   264,  /* (135) collate ::= */
   264,  /* (136) collate ::= COLLATE ID|STRING */
   267,  /* (137) with ::= */
   267,  /* (138) with ::= WITH wqlist */
   267,  /* (139) with ::= WITH RECURSIVE wqlist */
   207,  /* (140) cmd ::= with DELETE FROM xfullname indexed_opt where_opt_ret orderby_opt limit_opt */
   207,  /* (141) cmd ::= with UPDATE orconf xfullname indexed_opt SET setlist from where_opt_ret orderby_opt limit_opt */
   207,  /* (142) cmd ::= with insert_cmd INTO xfullname idlist_opt select upsert */
   207,  /* (143) cmd ::= with insert_cmd INTO xfullname idlist_opt DEFAULT VALUES returning */
   268,  /* (144) insert_cmd ::= INSERT orconf */
   268,  /* (145) insert_cmd ::= REPLACE */
   269,  /* (146) orconf ::= */
   269,  /* (147) orconf ::= OR resolvetype */
   261,  /* (148) resolvetype ::= raisetype */
   261,  /* (149) resolvetype ::= IGNORE */
   261,  /* (150) resolvetype ::= REPLACE */
   274,  /* (151) xfullname ::= nm */
   274,  /* (152) xfullname ::= nm DOT nm */
   274,  /* (153) xfullname ::= nm DOT nm AS nm */
   274,  /* (154) xfullname ::= nm AS nm */
   270,  /* (155) indexed_opt ::= */
   270,  /* (156) indexed_opt ::= indexed_by */
   271,  /* (157) where_opt_ret ::= */
   271,  /* (158) where_opt_ret ::= WHERE expr */
   271,  /* (159) where_opt_ret ::= RETURNING selcollist */
   271,  /* (160) where_opt_ret ::= WHERE expr RETURNING selcollist */
   277,  /* (161) setlist ::= setlist COMMA nm EQ expr */
   277,  /* (162) setlist ::= setlist COMMA LP idlist RP EQ expr */
   277,  /* (163) setlist ::= nm EQ expr */
   277,  /* (164) setlist ::= LP idlist RP EQ expr */
   279,  /* (165) idlist_opt ::= */
   279,  /* (166) idlist_opt ::= LP idlist RP */
   272,  /* (167) upsert ::= */
   272,  /* (168) upsert ::= RETURNING selcollist */
   272,  /* (169) upsert ::= ON CONFLICT LP sortlist RP where_opt DO UPDATE SET setlist where_opt upsert */
   272,  /* (170) upsert ::= ON CONFLICT LP sortlist RP where_opt DO NOTHING upsert */
   272,  /* (171) upsert ::= ON CONFLICT DO NOTHING returning */
   272,  /* (172) upsert ::= ON CONFLICT DO UPDATE SET setlist where_opt returning */
   273,  /* (173) returning ::= RETURNING selcollist */
   273,  /* (174) returning ::= */
   208,  /* (175) expr ::= error */
   208,  /* (176) expr ::= term */
   208,  /* (177) expr ::= LP expr RP */
   208,  /* (178) expr ::= expr PLUS|MINUS expr */
   208,  /* (179) expr ::= expr STAR|SLASH|REM expr */
   208,  /* (180) expr ::= expr LT|GT|GE|LE expr */
   208,  /* (181) expr ::= expr EQ|NE expr */
   208,  /* (182) expr ::= expr AND expr */
   208,  /* (183) expr ::= expr OR expr */
   208,  /* (184) expr ::= expr BITAND|BITOR|LSHIFT|RSHIFT expr */
   208,  /* (185) expr ::= expr CONCAT expr */
   208,  /* (186) expr ::= expr PTR expr */
   208,  /* (187) expr ::= PLUS|MINUS expr */
   208,  /* (188) expr ::= BITNOT expr */
   208,  /* (189) expr ::= NOT expr */
   210,  /* (190) exprlist ::= nexprlist */
   210,  /* (191) exprlist ::= */
   284,  /* (192) nexprlist ::= nexprlist COMMA expr */
   284,  /* (193) nexprlist ::= expr */
   208,  /* (194) expr ::= LP nexprlist COMMA expr RP */
   208,  /* (195) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP */
   208,  /* (196) expr ::= ID|INDEXED|JOIN_KW LP STAR RP */
   208,  /* (197) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP filter_over */
   208,  /* (198) expr ::= ID|INDEXED|JOIN_KW LP STAR RP filter_over */
   219,  /* (199) nm ::= ID|INDEXED|JOIN_KW */
   219,  /* (200) nm ::= STRING */
   285,  /* (201) nmorerr ::= nm */
   285,  /* (202) nmorerr ::= error */
   257,  /* (203) term ::= INTEGER */
   257,  /* (204) term ::= STRING */
   257,  /* (205) term ::= NULL|FLOAT|BLOB */
   257,  /* (206) term ::= QNUMBER */
   257,  /* (207) term ::= CTIME_KW */
   208,  /* (208) expr ::= VARIABLE */
   208,  /* (209) expr ::= expr COLLATE ID|STRING */
   211,  /* (210) sortlist ::= sortlist COMMA expr sortorder nulls */
   211,  /* (211) sortlist ::= expr sortorder nulls */
   258,  /* (212) sortorder ::= ASC */
   258,  /* (213) sortorder ::= DESC */
   258,  /* (214) sortorder ::= */
   286,  /* (215) nulls ::= NULLS FIRST */
   286,  /* (216) nulls ::= NULLS LAST */
   286,  /* (217) nulls ::= */
   208,  /* (218) expr ::= RAISE LP IGNORE RP */
   208,  /* (219) expr ::= RAISE LP raisetype COMMA expr RP */
   280,  /* (220) raisetype ::= ROLLBACK */
   280,  /* (221) raisetype ::= ABORT */
   280,  /* (222) raisetype ::= FAIL */
   292,  /* (223) fullname ::= nmorerr */
   292,  /* (224) fullname ::= nmorerr DOT nmorerr */
   287,  /* (225) ifexists ::= IF EXISTS */
   287,  /* (226) ifexists ::= */
   207,  /* (227) cmd ::= DROP TABLE ifexists fullname */
   207,  /* (228) cmd ::= DROP VIEW ifexists fullname */
   207,  /* (229) cmd ::= DROP INDEX ifexists fullname */
   207,  /* (230) cmd ::= DROP TRIGGER ifexists fullname */
   207,  /* (231) cmd ::= ALTER TABLE fullname RENAME TO nmorerr */
   207,  /* (232) cmd ::= ALTER TABLE fullname RENAME kwcolumn_opt nmorerr TO nmorerr */
   207,  /* (233) cmd ::= ALTER TABLE fullname DROP kwcolumn_opt nmorerr */
   207,  /* (234) cmd ::= ALTER TABLE add_column_fullname ADD kwcolumn_opt columnname carglist */
   293,  /* (235) add_column_fullname ::= fullname */
   291,  /* (236) kwcolumn_opt ::= */
   291,  /* (237) kwcolumn_opt ::= COLUMNKW */
   256,  /* (238) columnname ::= nmorerr typetoken */
   207,  /* (239) cmd ::= BEGIN transtype trans_opt */
   207,  /* (240) cmd ::= COMMIT|END trans_opt */
   207,  /* (241) cmd ::= ROLLBACK trans_opt */
   288,  /* (242) transtype ::= */
   288,  /* (243) transtype ::= DEFERRED */
   288,  /* (244) transtype ::= IMMEDIATE */
   288,  /* (245) transtype ::= EXCLUSIVE */
   289,  /* (246) trans_opt ::= */
   289,  /* (247) trans_opt ::= TRANSACTION */
   289,  /* (248) trans_opt ::= TRANSACTION nm */
   290,  /* (249) savepoint_opt ::= SAVEPOINT */
   290,  /* (250) savepoint_opt ::= */
   207,  /* (251) cmd ::= SAVEPOINT nmorerr */
   207,  /* (252) cmd ::= RELEASE savepoint_opt nmorerr */
   207,  /* (253) cmd ::= ROLLBACK trans_opt TO savepoint_opt nmorerr */
   207,  /* (254) cmd ::= select */
   225,  /* (255) select ::= selectnowith */
   223,  /* (256) selectnowith ::= oneselect */
   224,  /* (257) oneselect ::= SELECT distinct selcollist from where_opt groupby_opt having_opt orderby_opt limit_opt */
   224,  /* (258) oneselect ::= SELECT distinct selcollist from where_opt groupby_opt having_opt window_clause orderby_opt limit_opt */
   216,  /* (259) selcollist ::= sclp scanpt expr scanpt as */
   216,  /* (260) selcollist ::= sclp scanpt STAR */
   217,  /* (261) sclp ::= selcollist COMMA */
   217,  /* (262) sclp ::= */
   218,  /* (263) scanpt ::= */
   294,  /* (264) as ::= AS nmorerr */
   294,  /* (265) as ::= ID|STRING */
   294,  /* (266) as ::= */
   209,  /* (267) distinct ::= DISTINCT */
   209,  /* (268) distinct ::= ALL */
   209,  /* (269) distinct ::= */
   278,  /* (270) from ::= */
   278,  /* (271) from ::= FROM seltablist */
   283,  /* (272) where_opt ::= */
   283,  /* (273) where_opt ::= WHERE expr */
   295,  /* (274) groupby_opt ::= */
   295,  /* (275) groupby_opt ::= GROUP BY nexprlist */
   296,  /* (276) having_opt ::= */
   296,  /* (277) having_opt ::= HAVING expr */
   275,  /* (278) orderby_opt ::= */
   275,  /* (279) orderby_opt ::= ORDER BY sortlist */
   276,  /* (280) limit_opt ::= */
   276,  /* (281) limit_opt ::= LIMIT expr */
   276,  /* (282) limit_opt ::= LIMIT expr OFFSET expr */
   276,  /* (283) limit_opt ::= LIMIT expr COMMA expr */
   301,  /* (284) stl_prefix ::= seltablist joinop */
   301,  /* (285) stl_prefix ::= */
   298,  /* (286) seltablist ::= stl_prefix nm dbnm as on_using */
   298,  /* (287) seltablist ::= stl_prefix nm dbnm as indexed_by on_using */
   298,  /* (288) seltablist ::= stl_prefix nm dbnm LP exprlist RP as on_using */
   298,  /* (289) seltablist ::= stl_prefix LP select RP as on_using */
   298,  /* (290) seltablist ::= stl_prefix LP seltablist RP as on_using */
   300,  /* (291) joinop ::= COMMA|JOIN */
   300,  /* (292) joinop ::= JOIN_KW JOIN */
   300,  /* (293) joinop ::= JOIN_KW nm JOIN */
   300,  /* (294) joinop ::= JOIN_KW nm nm JOIN */
   299,  /* (295) on_using ::= ON expr */
   299,  /* (296) on_using ::= USING LP idlist RP */
   299,  /* (297) on_using ::= */
   281,  /* (298) indexed_by ::= INDEXED BY nm */
   281,  /* (299) indexed_by ::= NOT INDEXED */
   282,  /* (300) idlist ::= idlist COMMA nm */
   282,  /* (301) idlist ::= nm */
   207,  /* (302) cmd ::= createkw trigger_decl BEGIN trigger_cmd_list END */
   305,  /* (303) trigger_decl ::= temp TRIGGER ifnotexists nm dbnm trigger_time trigger_event ON fullname foreach_clause when_clause */
   302,  /* (304) trigger_time ::= BEFORE|AFTER */
   302,  /* (305) trigger_time ::= INSTEAD OF */
   302,  /* (306) trigger_time ::= */
   307,  /* (307) trigger_event ::= DELETE|INSERT */
   307,  /* (308) trigger_event ::= UPDATE */
   307,  /* (309) trigger_event ::= UPDATE OF idlist */
   303,  /* (310) foreach_clause ::= */
   303,  /* (311) foreach_clause ::= FOR EACH ROW */
   308,  /* (312) when_clause ::= */
   308,  /* (313) when_clause ::= WHEN expr */
   306,  /* (314) trigger_cmd_list ::= trigger_cmd_list trigger_cmd SEMI */
   306,  /* (315) trigger_cmd_list ::= trigger_cmd SEMI */
   304,  /* (316) trnm ::= nm */
   304,  /* (317) trnm ::= nm DOT nm */
   310,  /* (318) tridxby ::= */
   310,  /* (319) tridxby ::= INDEXED BY nm */
   310,  /* (320) tridxby ::= NOT INDEXED */
   309,  /* (321) trigger_cmd ::= UPDATE orconf trnm tridxby SET setlist from where_opt scanpt */
   309,  /* (322) trigger_cmd ::= scanpt insert_cmd INTO trnm idlist_opt select upsert scanpt */
   309,  /* (323) trigger_cmd ::= DELETE FROM trnm tridxby where_opt scanpt */
   309,  /* (324) trigger_cmd ::= scanpt select scanpt */
   207,  /* (325) cmd ::= PRAGMA nm dbnm */
   207,  /* (326) cmd ::= PRAGMA nm dbnm EQ nmnum */
   207,  /* (327) cmd ::= PRAGMA nm dbnm LP nmnum RP */
   207,  /* (328) cmd ::= PRAGMA nm dbnm EQ minus_num */
   207,  /* (329) cmd ::= PRAGMA nm dbnm LP minus_num RP */
   314,  /* (330) nmnum ::= plus_num */
   314,  /* (331) nmnum ::= nm */
   314,  /* (332) nmnum ::= ON */
   314,  /* (333) nmnum ::= DELETE */
   314,  /* (334) nmnum ::= DEFAULT */
   312,  /* (335) plus_num ::= PLUS INTEGER|FLOAT */
   312,  /* (336) plus_num ::= INTEGER|FLOAT */
   313,  /* (337) minus_num ::= MINUS INTEGER|FLOAT */
   215,  /* (338) signed ::= plus_num */
   215,  /* (339) signed ::= minus_num */
   207,  /* (340) cmd ::= ANALYZE */
   207,  /* (341) cmd ::= ANALYZE nm dbnm */
   207,  /* (342) cmd ::= REINDEX */
   207,  /* (343) cmd ::= REINDEX nm dbnm */
   207,  /* (344) cmd ::= ATTACH database_kw_opt expr AS expr key_opt */
   207,  /* (345) cmd ::= DETACH database_kw_opt expr */
   311,  /* (346) database_kw_opt ::= DATABASE */
   311,  /* (347) database_kw_opt ::= */
   317,  /* (348) key_opt ::= */
   317,  /* (349) key_opt ::= KEY expr */
   207,  /* (350) cmd ::= VACUUM vinto */
   207,  /* (351) cmd ::= VACUUM nm vinto */
   318,  /* (352) vinto ::= INTO expr */
   318,  /* (353) vinto ::= */
   204,  /* (354) ecmd ::= explain cmdx SEMI */
   316,  /* (355) explain ::= EXPLAIN */
   316,  /* (356) explain ::= EXPLAIN QUERY PLAN */
   207,  /* (357) cmd ::= createkw uniqueflag INDEX ifnotexists nm dbnm ON nm LP sortlist RP where_opt */
   315,  /* (358) uniqueflag ::= UNIQUE */
   315,  /* (359) uniqueflag ::= */
   253,  /* (360) ifnotexists ::= */
   253,  /* (361) ifnotexists ::= IF NOT EXISTS */
   207,  /* (362) cmd ::= createkw temp VIEW ifnotexists nm dbnm eidlist_opt AS select */
   251,  /* (363) createkw ::= CREATE */
   252,  /* (364) temp ::= TEMP */
   252,  /* (365) temp ::= */
   319,  /* (366) values ::= VALUES LP nexprlist RP */
   320,  /* (367) mvalues ::= values COMMA LP nexprlist RP */
   320,  /* (368) mvalues ::= mvalues COMMA LP nexprlist RP */
   224,  /* (369) oneselect ::= values */
   224,  /* (370) oneselect ::= mvalues */
   207,  /* (371) cmd ::= create_vtab */
   207,  /* (372) cmd ::= create_vtab LP vtabarglist RP */
   321,  /* (373) create_vtab ::= createkw VIRTUAL TABLE ifnotexists nm dbnm USING nm */
   322,  /* (374) vtabarglist ::= vtabarg */
   322,  /* (375) vtabarglist ::= vtabarglist COMMA vtabarg */
   323,  /* (376) vtabarg ::= */
   323,  /* (377) vtabarg ::= vtabarg vtabargtoken */
   324,  /* (378) vtabargtoken ::= ANY */
   324,  /* (379) vtabargtoken ::= lp anylist RP */
   325,  /* (380) lp ::= LP */
   326,  /* (381) anylist ::= */
   326,  /* (382) anylist ::= anylist LP anylist RP */
   326,  /* (383) anylist ::= anylist ANY */
   330,  /* (384) windowdefn_list ::= windowdefn */
   330,  /* (385) windowdefn_list ::= windowdefn_list COMMA windowdefn */
   331,  /* (386) windowdefn ::= nm AS LP window RP */
   332,  /* (387) window ::= PARTITION BY nexprlist orderby_opt frame_opt */
   332,  /* (388) window ::= nm PARTITION BY nexprlist orderby_opt frame_opt */
   332,  /* (389) window ::= ORDER BY sortlist frame_opt */
   332,  /* (390) window ::= nm ORDER BY sortlist frame_opt */
   332,  /* (391) window ::= frame_opt */
   332,  /* (392) window ::= nm frame_opt */
   333,  /* (393) frame_opt ::= */
   333,  /* (394) frame_opt ::= range_or_rows frame_bound_s frame_exclude_opt */
   333,  /* (395) frame_opt ::= range_or_rows BETWEEN frame_bound_s AND frame_bound_e frame_exclude_opt */
   327,  /* (396) range_or_rows ::= RANGE|ROWS|GROUPS */
   334,  /* (397) frame_bound_s ::= frame_bound */
   334,  /* (398) frame_bound_s ::= UNBOUNDED PRECEDING */
   335,  /* (399) frame_bound_e ::= frame_bound */
   335,  /* (400) frame_bound_e ::= UNBOUNDED FOLLOWING */
   336,  /* (401) frame_bound ::= expr PRECEDING|FOLLOWING */
   336,  /* (402) frame_bound ::= CURRENT ROW */
   328,  /* (403) frame_exclude_opt ::= */
   328,  /* (404) frame_exclude_opt ::= EXCLUDE frame_exclude */
   329,  /* (405) frame_exclude ::= NO OTHERS */
   329,  /* (406) frame_exclude ::= CURRENT ROW */
   329,  /* (407) frame_exclude ::= GROUP|TIES */
   297,  /* (408) window_clause ::= WINDOW windowdefn_list */
   212,  /* (409) filter_over ::= filter_clause over_clause */
   212,  /* (410) filter_over ::= over_clause */
   212,  /* (411) filter_over ::= filter_clause */
   338,  /* (412) over_clause ::= OVER LP window RP */
   338,  /* (413) over_clause ::= OVER nm */
   337,  /* (414) filter_clause ::= FILTER LP WHERE expr RP */
   339,  /* (415) perfetto_or_replace ::= */
   339,  /* (416) perfetto_or_replace ::= OR REPLACE */
   340,  /* (417) perfetto_arg_type ::= ID */
   340,  /* (418) perfetto_arg_type ::= ID LP ID DOT ID RP */
   341,  /* (419) perfetto_arg_def_list ::= */
   341,  /* (420) perfetto_arg_def_list ::= perfetto_arg_def_list_ne */
   342,  /* (421) perfetto_arg_def_list_ne ::= ID perfetto_arg_type */
   342,  /* (422) perfetto_arg_def_list_ne ::= perfetto_arg_def_list_ne COMMA ID perfetto_arg_type */
   342,  /* (423) perfetto_arg_def_list_ne ::= ID perfetto_arg_type DOT DOT DOT */
   342,  /* (424) perfetto_arg_def_list_ne ::= perfetto_arg_def_list_ne COMMA ID perfetto_arg_type DOT DOT DOT */
   343,  /* (425) perfetto_table_schema ::= */
   343,  /* (426) perfetto_table_schema ::= LP perfetto_arg_def_list_ne RP */
   344,  /* (427) perfetto_table_impl ::= */
   344,  /* (428) perfetto_table_impl ::= USING ID */
   345,  /* (429) perfetto_return_type ::= ID */
   345,  /* (430) perfetto_return_type ::= TABLE LP perfetto_arg_def_list_ne RP */
   346,  /* (431) perfetto_indexed_col_list ::= ID */
   346,  /* (432) perfetto_indexed_col_list ::= perfetto_indexed_col_list COMMA ID */
   347,  /* (433) perfetto_macro_arg_list ::= */
   347,  /* (434) perfetto_macro_arg_list ::= perfetto_macro_arg_list_ne */
   348,  /* (435) perfetto_macro_arg_list_ne ::= ID ID */
   348,  /* (436) perfetto_macro_arg_list_ne ::= perfetto_macro_arg_list_ne COMMA ID ID */
   349,  /* (437) perfetto_module_name ::= ID|STAR|INTERSECT */
   349,  /* (438) perfetto_module_name ::= perfetto_module_name DOT ID|STAR|INTERSECT */
   350,  /* (439) select_body_start ::= */
   351,  /* (440) select_body_end ::= */
   352,  /* (441) perfetto_pipe ::= BITOR GT */
   353,  /* (442) perfetto_pipe_source ::= nm dbnm as */
   353,  /* (443) perfetto_pipe_source ::= LP select RP as */
   354,  /* (444) perfetto_tree_direction ::= UP */
   354,  /* (445) perfetto_tree_direction ::= DOWN */
   355,  /* (446) perfetto_tree_aggregate ::= expr AS nm */
   356,  /* (447) perfetto_tree_aggregate_list ::= perfetto_tree_aggregate */
   356,  /* (448) perfetto_tree_aggregate_list ::= perfetto_tree_aggregate_list COMMA perfetto_tree_aggregate */
   357,  /* (449) perfetto_pipe_select_item ::= nm */
   357,  /* (450) perfetto_pipe_select_item ::= nm AS nm */
   357,  /* (451) perfetto_pipe_select_item ::= nm DOT nm */
   357,  /* (452) perfetto_pipe_select_item ::= nm DOT nm AS nm */
   358,  /* (453) perfetto_pipe_select_list ::= perfetto_pipe_select_item */
   358,  /* (454) perfetto_pipe_select_list ::= perfetto_pipe_select_list COMMA perfetto_pipe_select_item */
   359,  /* (455) perfetto_pipe_stage ::= SELECT perfetto_pipe_select_list */
   359,  /* (456) perfetto_pipe_stage ::= TREE ACCUMULATE perfetto_tree_direction perfetto_tree_aggregate_list */
   360,  /* (457) perfetto_pipe_stage_list ::= */
   360,  /* (458) perfetto_pipe_stage_list ::= perfetto_pipe_stage_list perfetto_pipe perfetto_pipe_stage */
   361,  /* (459) perfetto_pipe_source_list ::= perfetto_pipe_source */
   361,  /* (460) perfetto_pipe_source_list ::= perfetto_pipe_source_list COMMA perfetto_pipe_source */
   362,  /* (461) perfetto_per_col_list ::= nm */
   362,  /* (462) perfetto_per_col_list ::= perfetto_per_col_list COMMA nm */
   363,  /* (463) perfetto_per ::= */
   363,  /* (464) perfetto_per ::= PER perfetto_per_col_list */
   364,  /* (465) perfetto_pipeline ::= FROM perfetto_pipe_source perfetto_pipe_stage_list */
   364,  /* (466) perfetto_pipeline ::= INTERVAL INTERSECTION OF LP perfetto_pipe_source_list RP perfetto_per perfetto_pipe_stage_list */
   207,  /* (467) cmd ::= perfetto_pipeline */
   207,  /* (468) cmd ::= PERFETTO PRAGMA nm EQ expr */
   207,  /* (469) cmd ::= CREATE perfetto_or_replace PERFETTO TABLE nm perfetto_table_impl perfetto_table_schema AS select_body_start select select_body_end */
   207,  /* (470) cmd ::= CREATE perfetto_or_replace PERFETTO TABLE nm perfetto_table_impl perfetto_table_schema AS perfetto_pipeline */
   207,  /* (471) cmd ::= CREATE perfetto_or_replace PERFETTO VIEW nm perfetto_table_schema AS select_body_start select select_body_end */
   207,  /* (472) cmd ::= CREATE perfetto_or_replace PERFETTO FUNCTION nm LP perfetto_arg_def_list RP RETURNS perfetto_return_type AS select_body_start select select_body_end */
   207,  /* (473) cmd ::= CREATE perfetto_or_replace PERFETTO FUNCTION nm LP perfetto_arg_def_list RP RETURNS perfetto_return_type DELEGATES TO ID */
   207,  /* (474) cmd ::= CREATE perfetto_or_replace PERFETTO INDEX nm ON nm LP perfetto_indexed_col_list RP */
   365,  /* (475) before_macro_body ::= */
   366,  /* (476) perfetto_macro_body ::= ANY */
   366,  /* (477) perfetto_macro_body ::= perfetto_macro_body ANY */
   207,  /* (478) cmd ::= CREATE perfetto_or_replace PERFETTO MACRO nm LP perfetto_macro_arg_list RP RETURNS ID before_macro_body AS perfetto_macro_body */
   207,  /* (479) cmd ::= INCLUDE PERFETTO MODULE perfetto_module_name */
   207,  /* (480) cmd ::= DROP PERFETTO INDEX nm ON nm */
};

/* For rule J, yyRuleInfoNRhs[J] contains the negative of the number
** of symbols on the right-hand side of that rule. */
static const signed char yyRuleInfoNRhs[] = {
   -1,  /* (0) input ::= cmdlist */
   -2,  /* (1) cmdlist ::= cmdlist ecmd */
   -1,  /* (2) cmdlist ::= ecmd */
   -1,  /* (3) ecmd ::= SEMI */
   -2,  /* (4) ecmd ::= cmdx SEMI */
   -2,  /* (5) ecmd ::= error SEMI */
   -1,  /* (6) cmdx ::= cmd */
   -8,  /* (7) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist ORDER BY sortlist RP */
   -9,  /* (8) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist ORDER BY sortlist RP filter_over */
  -12,  /* (9) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP WITHIN GROUP LP ORDER BY expr RP */
  -13,  /* (10) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP WITHIN GROUP LP ORDER BY expr RP filter_over */
   -6,  /* (11) expr ::= CAST LP expr AS typetoken RP */
    0,  /* (12) typetoken ::= */
   -1,  /* (13) typetoken ::= typename */
   -4,  /* (14) typetoken ::= typename LP signed RP */
   -6,  /* (15) typetoken ::= typename LP signed COMMA signed RP */
   -1,  /* (16) typename ::= ID|STRING */
   -2,  /* (17) typename ::= typename ID|STRING */
   -5,  /* (18) selcollist ::= sclp scanpt nm DOT STAR */
   -1,  /* (19) expr ::= ID|INDEXED|JOIN_KW */
   -3,  /* (20) expr ::= nm DOT nm */
   -5,  /* (21) expr ::= nm DOT nm DOT nm */
   -3,  /* (22) selectnowith ::= selectnowith multiselect_op oneselect */
   -1,  /* (23) multiselect_op ::= UNION */
   -2,  /* (24) multiselect_op ::= UNION ALL */
   -1,  /* (25) multiselect_op ::= EXCEPT|INTERSECT */
   -3,  /* (26) expr ::= LP select RP */
   -4,  /* (27) expr ::= EXISTS LP select RP */
   -1,  /* (28) in_op ::= IN */
   -2,  /* (29) in_op ::= NOT IN */
   -5,  /* (30) expr ::= expr in_op LP exprlist RP */
   -5,  /* (31) expr ::= expr in_op LP select RP */
   -5,  /* (32) expr ::= expr in_op nm dbnm paren_exprlist */
    0,  /* (33) dbnm ::= */
   -2,  /* (34) dbnm ::= DOT nm */
    0,  /* (35) paren_exprlist ::= */
   -3,  /* (36) paren_exprlist ::= LP exprlist RP */
   -2,  /* (37) expr ::= expr ISNULL|NOTNULL */
   -3,  /* (38) expr ::= expr NOT NULL */
   -3,  /* (39) expr ::= expr IS expr */
   -4,  /* (40) expr ::= expr IS NOT expr */
   -6,  /* (41) expr ::= expr IS NOT DISTINCT FROM expr */
   -5,  /* (42) expr ::= expr IS DISTINCT FROM expr */
   -1,  /* (43) between_op ::= BETWEEN */
   -2,  /* (44) between_op ::= NOT BETWEEN */
   -5,  /* (45) expr ::= expr between_op expr AND expr */
   -1,  /* (46) likeop ::= LIKE_KW|MATCH */
   -2,  /* (47) likeop ::= NOT LIKE_KW|MATCH */
   -3,  /* (48) expr ::= expr likeop expr */
   -5,  /* (49) expr ::= expr likeop expr ESCAPE expr */
   -5,  /* (50) expr ::= CASE case_operand case_exprlist case_else END */
   -5,  /* (51) case_exprlist ::= case_exprlist WHEN expr THEN expr */
   -4,  /* (52) case_exprlist ::= WHEN expr THEN expr */
   -2,  /* (53) case_else ::= ELSE expr */
    0,  /* (54) case_else ::= */
   -1,  /* (55) case_operand ::= expr */
    0,  /* (56) case_operand ::= */
   -2,  /* (57) cmd ::= create_table create_table_args */
   -6,  /* (58) create_table ::= createkw temp TABLE ifnotexists nm dbnm */
   -5,  /* (59) create_table_args ::= LP columnlist conslist_opt RP table_option_set */
   -2,  /* (60) create_table_args ::= AS select */
    0,  /* (61) table_option_set ::= */
   -1,  /* (62) table_option_set ::= table_option */
   -3,  /* (63) table_option_set ::= table_option_set COMMA table_option */
   -2,  /* (64) table_option ::= WITHOUT nm */
   -1,  /* (65) table_option ::= nm */
   -4,  /* (66) columnlist ::= columnlist COMMA columnname carglist */
   -2,  /* (67) columnlist ::= columnname carglist */
   -2,  /* (68) carglist ::= carglist ccons */
    0,  /* (69) carglist ::= */
   -2,  /* (70) ccons ::= CONSTRAINT nm */
   -3,  /* (71) ccons ::= DEFAULT scantok term */
   -4,  /* (72) ccons ::= DEFAULT LP expr RP */
   -4,  /* (73) ccons ::= DEFAULT PLUS scantok term */
   -4,  /* (74) ccons ::= DEFAULT MINUS scantok term */
   -3,  /* (75) ccons ::= DEFAULT scantok ID|INDEXED */
   -2,  /* (76) ccons ::= NULL onconf */
   -3,  /* (77) ccons ::= NOT NULL onconf */
   -5,  /* (78) ccons ::= PRIMARY KEY sortorder onconf autoinc */
   -2,  /* (79) ccons ::= UNIQUE onconf */
   -4,  /* (80) ccons ::= CHECK LP expr RP */
   -4,  /* (81) ccons ::= REFERENCES nm eidlist_opt refargs */
   -1,  /* (82) ccons ::= defer_subclause */
   -2,  /* (83) ccons ::= COLLATE ID|STRING */
   -4,  /* (84) ccons ::= GENERATED ALWAYS AS generated */
   -2,  /* (85) ccons ::= AS generated */
   -3,  /* (86) generated ::= LP expr RP */
   -4,  /* (87) generated ::= LP expr RP ID */
    0,  /* (88) autoinc ::= */
   -1,  /* (89) autoinc ::= AUTOINCR */
    0,  /* (90) refargs ::= */
   -2,  /* (91) refargs ::= refargs refarg */
   -2,  /* (92) refarg ::= MATCH nm */
   -3,  /* (93) refarg ::= ON INSERT refact */
   -3,  /* (94) refarg ::= ON DELETE refact */
   -3,  /* (95) refarg ::= ON UPDATE refact */
   -2,  /* (96) refact ::= SET NULL */
   -2,  /* (97) refact ::= SET DEFAULT */
   -1,  /* (98) refact ::= CASCADE */
   -1,  /* (99) refact ::= RESTRICT */
   -2,  /* (100) refact ::= NO ACTION */
   -3,  /* (101) defer_subclause ::= NOT DEFERRABLE init_deferred_pred_opt */
   -2,  /* (102) defer_subclause ::= DEFERRABLE init_deferred_pred_opt */
    0,  /* (103) init_deferred_pred_opt ::= */
   -2,  /* (104) init_deferred_pred_opt ::= INITIALLY DEFERRED */
   -2,  /* (105) init_deferred_pred_opt ::= INITIALLY IMMEDIATE */
    0,  /* (106) conslist_opt ::= */
   -2,  /* (107) conslist_opt ::= COMMA conslist */
   -3,  /* (108) conslist ::= conslist tconscomma tcons */
   -1,  /* (109) conslist ::= tcons */
   -1,  /* (110) tconscomma ::= COMMA */
    0,  /* (111) tconscomma ::= */
   -2,  /* (112) tcons ::= CONSTRAINT nm */
   -7,  /* (113) tcons ::= PRIMARY KEY LP sortlist autoinc RP onconf */
   -5,  /* (114) tcons ::= UNIQUE LP sortlist RP onconf */
   -5,  /* (115) tcons ::= CHECK LP expr RP onconf */
  -10,  /* (116) tcons ::= FOREIGN KEY LP eidlist RP REFERENCES nm eidlist_opt refargs defer_subclause_opt */
    0,  /* (117) defer_subclause_opt ::= */
   -1,  /* (118) defer_subclause_opt ::= defer_subclause */
    0,  /* (119) onconf ::= */
   -3,  /* (120) onconf ::= ON CONFLICT resolvetype */
    0,  /* (121) scantok ::= */
   -3,  /* (122) select ::= WITH wqlist selectnowith */
   -4,  /* (123) select ::= WITH RECURSIVE wqlist selectnowith */
   -6,  /* (124) wqitem ::= withnm eidlist_opt wqas LP select RP */
   -1,  /* (125) wqlist ::= wqitem */
   -3,  /* (126) wqlist ::= wqlist COMMA wqitem */
   -1,  /* (127) withnm ::= nm */
   -1,  /* (128) wqas ::= AS */
   -2,  /* (129) wqas ::= AS MATERIALIZED */
   -3,  /* (130) wqas ::= AS NOT MATERIALIZED */
    0,  /* (131) eidlist_opt ::= */
   -3,  /* (132) eidlist_opt ::= LP eidlist RP */
   -3,  /* (133) eidlist ::= nm collate sortorder */
   -5,  /* (134) eidlist ::= eidlist COMMA nm collate sortorder */
    0,  /* (135) collate ::= */
   -2,  /* (136) collate ::= COLLATE ID|STRING */
    0,  /* (137) with ::= */
   -2,  /* (138) with ::= WITH wqlist */
   -3,  /* (139) with ::= WITH RECURSIVE wqlist */
   -8,  /* (140) cmd ::= with DELETE FROM xfullname indexed_opt where_opt_ret orderby_opt limit_opt */
  -11,  /* (141) cmd ::= with UPDATE orconf xfullname indexed_opt SET setlist from where_opt_ret orderby_opt limit_opt */
   -7,  /* (142) cmd ::= with insert_cmd INTO xfullname idlist_opt select upsert */
   -8,  /* (143) cmd ::= with insert_cmd INTO xfullname idlist_opt DEFAULT VALUES returning */
   -2,  /* (144) insert_cmd ::= INSERT orconf */
   -1,  /* (145) insert_cmd ::= REPLACE */
    0,  /* (146) orconf ::= */
   -2,  /* (147) orconf ::= OR resolvetype */
   -1,  /* (148) resolvetype ::= raisetype */
   -1,  /* (149) resolvetype ::= IGNORE */
   -1,  /* (150) resolvetype ::= REPLACE */
   -1,  /* (151) xfullname ::= nm */
   -3,  /* (152) xfullname ::= nm DOT nm */
   -5,  /* (153) xfullname ::= nm DOT nm AS nm */
   -3,  /* (154) xfullname ::= nm AS nm */
    0,  /* (155) indexed_opt ::= */
   -1,  /* (156) indexed_opt ::= indexed_by */
    0,  /* (157) where_opt_ret ::= */
   -2,  /* (158) where_opt_ret ::= WHERE expr */
   -2,  /* (159) where_opt_ret ::= RETURNING selcollist */
   -4,  /* (160) where_opt_ret ::= WHERE expr RETURNING selcollist */
   -5,  /* (161) setlist ::= setlist COMMA nm EQ expr */
   -7,  /* (162) setlist ::= setlist COMMA LP idlist RP EQ expr */
   -3,  /* (163) setlist ::= nm EQ expr */
   -5,  /* (164) setlist ::= LP idlist RP EQ expr */
    0,  /* (165) idlist_opt ::= */
   -3,  /* (166) idlist_opt ::= LP idlist RP */
    0,  /* (167) upsert ::= */
   -2,  /* (168) upsert ::= RETURNING selcollist */
  -12,  /* (169) upsert ::= ON CONFLICT LP sortlist RP where_opt DO UPDATE SET setlist where_opt upsert */
   -9,  /* (170) upsert ::= ON CONFLICT LP sortlist RP where_opt DO NOTHING upsert */
   -5,  /* (171) upsert ::= ON CONFLICT DO NOTHING returning */
   -8,  /* (172) upsert ::= ON CONFLICT DO UPDATE SET setlist where_opt returning */
   -2,  /* (173) returning ::= RETURNING selcollist */
    0,  /* (174) returning ::= */
   -1,  /* (175) expr ::= error */
   -1,  /* (176) expr ::= term */
   -3,  /* (177) expr ::= LP expr RP */
   -3,  /* (178) expr ::= expr PLUS|MINUS expr */
   -3,  /* (179) expr ::= expr STAR|SLASH|REM expr */
   -3,  /* (180) expr ::= expr LT|GT|GE|LE expr */
   -3,  /* (181) expr ::= expr EQ|NE expr */
   -3,  /* (182) expr ::= expr AND expr */
   -3,  /* (183) expr ::= expr OR expr */
   -3,  /* (184) expr ::= expr BITAND|BITOR|LSHIFT|RSHIFT expr */
   -3,  /* (185) expr ::= expr CONCAT expr */
   -3,  /* (186) expr ::= expr PTR expr */
   -2,  /* (187) expr ::= PLUS|MINUS expr */
   -2,  /* (188) expr ::= BITNOT expr */
   -2,  /* (189) expr ::= NOT expr */
   -1,  /* (190) exprlist ::= nexprlist */
    0,  /* (191) exprlist ::= */
   -3,  /* (192) nexprlist ::= nexprlist COMMA expr */
   -1,  /* (193) nexprlist ::= expr */
   -5,  /* (194) expr ::= LP nexprlist COMMA expr RP */
   -5,  /* (195) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP */
   -4,  /* (196) expr ::= ID|INDEXED|JOIN_KW LP STAR RP */
   -6,  /* (197) expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP filter_over */
   -5,  /* (198) expr ::= ID|INDEXED|JOIN_KW LP STAR RP filter_over */
   -1,  /* (199) nm ::= ID|INDEXED|JOIN_KW */
   -1,  /* (200) nm ::= STRING */
   -1,  /* (201) nmorerr ::= nm */
   -1,  /* (202) nmorerr ::= error */
   -1,  /* (203) term ::= INTEGER */
   -1,  /* (204) term ::= STRING */
   -1,  /* (205) term ::= NULL|FLOAT|BLOB */
   -1,  /* (206) term ::= QNUMBER */
   -1,  /* (207) term ::= CTIME_KW */
   -1,  /* (208) expr ::= VARIABLE */
   -3,  /* (209) expr ::= expr COLLATE ID|STRING */
   -5,  /* (210) sortlist ::= sortlist COMMA expr sortorder nulls */
   -3,  /* (211) sortlist ::= expr sortorder nulls */
   -1,  /* (212) sortorder ::= ASC */
   -1,  /* (213) sortorder ::= DESC */
    0,  /* (214) sortorder ::= */
   -2,  /* (215) nulls ::= NULLS FIRST */
   -2,  /* (216) nulls ::= NULLS LAST */
    0,  /* (217) nulls ::= */
   -4,  /* (218) expr ::= RAISE LP IGNORE RP */
   -6,  /* (219) expr ::= RAISE LP raisetype COMMA expr RP */
   -1,  /* (220) raisetype ::= ROLLBACK */
   -1,  /* (221) raisetype ::= ABORT */
   -1,  /* (222) raisetype ::= FAIL */
   -1,  /* (223) fullname ::= nmorerr */
   -3,  /* (224) fullname ::= nmorerr DOT nmorerr */
   -2,  /* (225) ifexists ::= IF EXISTS */
    0,  /* (226) ifexists ::= */
   -4,  /* (227) cmd ::= DROP TABLE ifexists fullname */
   -4,  /* (228) cmd ::= DROP VIEW ifexists fullname */
   -4,  /* (229) cmd ::= DROP INDEX ifexists fullname */
   -4,  /* (230) cmd ::= DROP TRIGGER ifexists fullname */
   -6,  /* (231) cmd ::= ALTER TABLE fullname RENAME TO nmorerr */
   -8,  /* (232) cmd ::= ALTER TABLE fullname RENAME kwcolumn_opt nmorerr TO nmorerr */
   -6,  /* (233) cmd ::= ALTER TABLE fullname DROP kwcolumn_opt nmorerr */
   -7,  /* (234) cmd ::= ALTER TABLE add_column_fullname ADD kwcolumn_opt columnname carglist */
   -1,  /* (235) add_column_fullname ::= fullname */
    0,  /* (236) kwcolumn_opt ::= */
   -1,  /* (237) kwcolumn_opt ::= COLUMNKW */
   -2,  /* (238) columnname ::= nmorerr typetoken */
   -3,  /* (239) cmd ::= BEGIN transtype trans_opt */
   -2,  /* (240) cmd ::= COMMIT|END trans_opt */
   -2,  /* (241) cmd ::= ROLLBACK trans_opt */
    0,  /* (242) transtype ::= */
   -1,  /* (243) transtype ::= DEFERRED */
   -1,  /* (244) transtype ::= IMMEDIATE */
   -1,  /* (245) transtype ::= EXCLUSIVE */
    0,  /* (246) trans_opt ::= */
   -1,  /* (247) trans_opt ::= TRANSACTION */
   -2,  /* (248) trans_opt ::= TRANSACTION nm */
   -1,  /* (249) savepoint_opt ::= SAVEPOINT */
    0,  /* (250) savepoint_opt ::= */
   -2,  /* (251) cmd ::= SAVEPOINT nmorerr */
   -3,  /* (252) cmd ::= RELEASE savepoint_opt nmorerr */
   -5,  /* (253) cmd ::= ROLLBACK trans_opt TO savepoint_opt nmorerr */
   -1,  /* (254) cmd ::= select */
   -1,  /* (255) select ::= selectnowith */
   -1,  /* (256) selectnowith ::= oneselect */
   -9,  /* (257) oneselect ::= SELECT distinct selcollist from where_opt groupby_opt having_opt orderby_opt limit_opt */
  -10,  /* (258) oneselect ::= SELECT distinct selcollist from where_opt groupby_opt having_opt window_clause orderby_opt limit_opt */
   -5,  /* (259) selcollist ::= sclp scanpt expr scanpt as */
   -3,  /* (260) selcollist ::= sclp scanpt STAR */
   -2,  /* (261) sclp ::= selcollist COMMA */
    0,  /* (262) sclp ::= */
    0,  /* (263) scanpt ::= */
   -2,  /* (264) as ::= AS nmorerr */
   -1,  /* (265) as ::= ID|STRING */
    0,  /* (266) as ::= */
   -1,  /* (267) distinct ::= DISTINCT */
   -1,  /* (268) distinct ::= ALL */
    0,  /* (269) distinct ::= */
    0,  /* (270) from ::= */
   -2,  /* (271) from ::= FROM seltablist */
    0,  /* (272) where_opt ::= */
   -2,  /* (273) where_opt ::= WHERE expr */
    0,  /* (274) groupby_opt ::= */
   -3,  /* (275) groupby_opt ::= GROUP BY nexprlist */
    0,  /* (276) having_opt ::= */
   -2,  /* (277) having_opt ::= HAVING expr */
    0,  /* (278) orderby_opt ::= */
   -3,  /* (279) orderby_opt ::= ORDER BY sortlist */
    0,  /* (280) limit_opt ::= */
   -2,  /* (281) limit_opt ::= LIMIT expr */
   -4,  /* (282) limit_opt ::= LIMIT expr OFFSET expr */
   -4,  /* (283) limit_opt ::= LIMIT expr COMMA expr */
   -2,  /* (284) stl_prefix ::= seltablist joinop */
    0,  /* (285) stl_prefix ::= */
   -5,  /* (286) seltablist ::= stl_prefix nm dbnm as on_using */
   -6,  /* (287) seltablist ::= stl_prefix nm dbnm as indexed_by on_using */
   -8,  /* (288) seltablist ::= stl_prefix nm dbnm LP exprlist RP as on_using */
   -6,  /* (289) seltablist ::= stl_prefix LP select RP as on_using */
   -6,  /* (290) seltablist ::= stl_prefix LP seltablist RP as on_using */
   -1,  /* (291) joinop ::= COMMA|JOIN */
   -2,  /* (292) joinop ::= JOIN_KW JOIN */
   -3,  /* (293) joinop ::= JOIN_KW nm JOIN */
   -4,  /* (294) joinop ::= JOIN_KW nm nm JOIN */
   -2,  /* (295) on_using ::= ON expr */
   -4,  /* (296) on_using ::= USING LP idlist RP */
    0,  /* (297) on_using ::= */
   -3,  /* (298) indexed_by ::= INDEXED BY nm */
   -2,  /* (299) indexed_by ::= NOT INDEXED */
   -3,  /* (300) idlist ::= idlist COMMA nm */
   -1,  /* (301) idlist ::= nm */
   -5,  /* (302) cmd ::= createkw trigger_decl BEGIN trigger_cmd_list END */
  -11,  /* (303) trigger_decl ::= temp TRIGGER ifnotexists nm dbnm trigger_time trigger_event ON fullname foreach_clause when_clause */
   -1,  /* (304) trigger_time ::= BEFORE|AFTER */
   -2,  /* (305) trigger_time ::= INSTEAD OF */
    0,  /* (306) trigger_time ::= */
   -1,  /* (307) trigger_event ::= DELETE|INSERT */
   -1,  /* (308) trigger_event ::= UPDATE */
   -3,  /* (309) trigger_event ::= UPDATE OF idlist */
    0,  /* (310) foreach_clause ::= */
   -3,  /* (311) foreach_clause ::= FOR EACH ROW */
    0,  /* (312) when_clause ::= */
   -2,  /* (313) when_clause ::= WHEN expr */
   -3,  /* (314) trigger_cmd_list ::= trigger_cmd_list trigger_cmd SEMI */
   -2,  /* (315) trigger_cmd_list ::= trigger_cmd SEMI */
   -1,  /* (316) trnm ::= nm */
   -3,  /* (317) trnm ::= nm DOT nm */
    0,  /* (318) tridxby ::= */
   -3,  /* (319) tridxby ::= INDEXED BY nm */
   -2,  /* (320) tridxby ::= NOT INDEXED */
   -9,  /* (321) trigger_cmd ::= UPDATE orconf trnm tridxby SET setlist from where_opt scanpt */
   -8,  /* (322) trigger_cmd ::= scanpt insert_cmd INTO trnm idlist_opt select upsert scanpt */
   -6,  /* (323) trigger_cmd ::= DELETE FROM trnm tridxby where_opt scanpt */
   -3,  /* (324) trigger_cmd ::= scanpt select scanpt */
   -3,  /* (325) cmd ::= PRAGMA nm dbnm */
   -5,  /* (326) cmd ::= PRAGMA nm dbnm EQ nmnum */
   -6,  /* (327) cmd ::= PRAGMA nm dbnm LP nmnum RP */
   -5,  /* (328) cmd ::= PRAGMA nm dbnm EQ minus_num */
   -6,  /* (329) cmd ::= PRAGMA nm dbnm LP minus_num RP */
   -1,  /* (330) nmnum ::= plus_num */
   -1,  /* (331) nmnum ::= nm */
   -1,  /* (332) nmnum ::= ON */
   -1,  /* (333) nmnum ::= DELETE */
   -1,  /* (334) nmnum ::= DEFAULT */
   -2,  /* (335) plus_num ::= PLUS INTEGER|FLOAT */
   -1,  /* (336) plus_num ::= INTEGER|FLOAT */
   -2,  /* (337) minus_num ::= MINUS INTEGER|FLOAT */
   -1,  /* (338) signed ::= plus_num */
   -1,  /* (339) signed ::= minus_num */
   -1,  /* (340) cmd ::= ANALYZE */
   -3,  /* (341) cmd ::= ANALYZE nm dbnm */
   -1,  /* (342) cmd ::= REINDEX */
   -3,  /* (343) cmd ::= REINDEX nm dbnm */
   -6,  /* (344) cmd ::= ATTACH database_kw_opt expr AS expr key_opt */
   -3,  /* (345) cmd ::= DETACH database_kw_opt expr */
   -1,  /* (346) database_kw_opt ::= DATABASE */
    0,  /* (347) database_kw_opt ::= */
    0,  /* (348) key_opt ::= */
   -2,  /* (349) key_opt ::= KEY expr */
   -2,  /* (350) cmd ::= VACUUM vinto */
   -3,  /* (351) cmd ::= VACUUM nm vinto */
   -2,  /* (352) vinto ::= INTO expr */
    0,  /* (353) vinto ::= */
   -3,  /* (354) ecmd ::= explain cmdx SEMI */
   -1,  /* (355) explain ::= EXPLAIN */
   -3,  /* (356) explain ::= EXPLAIN QUERY PLAN */
  -12,  /* (357) cmd ::= createkw uniqueflag INDEX ifnotexists nm dbnm ON nm LP sortlist RP where_opt */
   -1,  /* (358) uniqueflag ::= UNIQUE */
    0,  /* (359) uniqueflag ::= */
    0,  /* (360) ifnotexists ::= */
   -3,  /* (361) ifnotexists ::= IF NOT EXISTS */
   -9,  /* (362) cmd ::= createkw temp VIEW ifnotexists nm dbnm eidlist_opt AS select */
   -1,  /* (363) createkw ::= CREATE */
   -1,  /* (364) temp ::= TEMP */
    0,  /* (365) temp ::= */
   -4,  /* (366) values ::= VALUES LP nexprlist RP */
   -5,  /* (367) mvalues ::= values COMMA LP nexprlist RP */
   -5,  /* (368) mvalues ::= mvalues COMMA LP nexprlist RP */
   -1,  /* (369) oneselect ::= values */
   -1,  /* (370) oneselect ::= mvalues */
   -1,  /* (371) cmd ::= create_vtab */
   -4,  /* (372) cmd ::= create_vtab LP vtabarglist RP */
   -8,  /* (373) create_vtab ::= createkw VIRTUAL TABLE ifnotexists nm dbnm USING nm */
   -1,  /* (374) vtabarglist ::= vtabarg */
   -3,  /* (375) vtabarglist ::= vtabarglist COMMA vtabarg */
    0,  /* (376) vtabarg ::= */
   -2,  /* (377) vtabarg ::= vtabarg vtabargtoken */
   -1,  /* (378) vtabargtoken ::= ANY */
   -3,  /* (379) vtabargtoken ::= lp anylist RP */
   -1,  /* (380) lp ::= LP */
    0,  /* (381) anylist ::= */
   -4,  /* (382) anylist ::= anylist LP anylist RP */
   -2,  /* (383) anylist ::= anylist ANY */
   -1,  /* (384) windowdefn_list ::= windowdefn */
   -3,  /* (385) windowdefn_list ::= windowdefn_list COMMA windowdefn */
   -5,  /* (386) windowdefn ::= nm AS LP window RP */
   -5,  /* (387) window ::= PARTITION BY nexprlist orderby_opt frame_opt */
   -6,  /* (388) window ::= nm PARTITION BY nexprlist orderby_opt frame_opt */
   -4,  /* (389) window ::= ORDER BY sortlist frame_opt */
   -5,  /* (390) window ::= nm ORDER BY sortlist frame_opt */
   -1,  /* (391) window ::= frame_opt */
   -2,  /* (392) window ::= nm frame_opt */
    0,  /* (393) frame_opt ::= */
   -3,  /* (394) frame_opt ::= range_or_rows frame_bound_s frame_exclude_opt */
   -6,  /* (395) frame_opt ::= range_or_rows BETWEEN frame_bound_s AND frame_bound_e frame_exclude_opt */
   -1,  /* (396) range_or_rows ::= RANGE|ROWS|GROUPS */
   -1,  /* (397) frame_bound_s ::= frame_bound */
   -2,  /* (398) frame_bound_s ::= UNBOUNDED PRECEDING */
   -1,  /* (399) frame_bound_e ::= frame_bound */
   -2,  /* (400) frame_bound_e ::= UNBOUNDED FOLLOWING */
   -2,  /* (401) frame_bound ::= expr PRECEDING|FOLLOWING */
   -2,  /* (402) frame_bound ::= CURRENT ROW */
    0,  /* (403) frame_exclude_opt ::= */
   -2,  /* (404) frame_exclude_opt ::= EXCLUDE frame_exclude */
   -2,  /* (405) frame_exclude ::= NO OTHERS */
   -2,  /* (406) frame_exclude ::= CURRENT ROW */
   -1,  /* (407) frame_exclude ::= GROUP|TIES */
   -2,  /* (408) window_clause ::= WINDOW windowdefn_list */
   -2,  /* (409) filter_over ::= filter_clause over_clause */
   -1,  /* (410) filter_over ::= over_clause */
   -1,  /* (411) filter_over ::= filter_clause */
   -4,  /* (412) over_clause ::= OVER LP window RP */
   -2,  /* (413) over_clause ::= OVER nm */
   -5,  /* (414) filter_clause ::= FILTER LP WHERE expr RP */
    0,  /* (415) perfetto_or_replace ::= */
   -2,  /* (416) perfetto_or_replace ::= OR REPLACE */
   -1,  /* (417) perfetto_arg_type ::= ID */
   -6,  /* (418) perfetto_arg_type ::= ID LP ID DOT ID RP */
    0,  /* (419) perfetto_arg_def_list ::= */
   -1,  /* (420) perfetto_arg_def_list ::= perfetto_arg_def_list_ne */
   -2,  /* (421) perfetto_arg_def_list_ne ::= ID perfetto_arg_type */
   -4,  /* (422) perfetto_arg_def_list_ne ::= perfetto_arg_def_list_ne COMMA ID perfetto_arg_type */
   -5,  /* (423) perfetto_arg_def_list_ne ::= ID perfetto_arg_type DOT DOT DOT */
   -7,  /* (424) perfetto_arg_def_list_ne ::= perfetto_arg_def_list_ne COMMA ID perfetto_arg_type DOT DOT DOT */
    0,  /* (425) perfetto_table_schema ::= */
   -3,  /* (426) perfetto_table_schema ::= LP perfetto_arg_def_list_ne RP */
    0,  /* (427) perfetto_table_impl ::= */
   -2,  /* (428) perfetto_table_impl ::= USING ID */
   -1,  /* (429) perfetto_return_type ::= ID */
   -4,  /* (430) perfetto_return_type ::= TABLE LP perfetto_arg_def_list_ne RP */
   -1,  /* (431) perfetto_indexed_col_list ::= ID */
   -3,  /* (432) perfetto_indexed_col_list ::= perfetto_indexed_col_list COMMA ID */
    0,  /* (433) perfetto_macro_arg_list ::= */
   -1,  /* (434) perfetto_macro_arg_list ::= perfetto_macro_arg_list_ne */
   -2,  /* (435) perfetto_macro_arg_list_ne ::= ID ID */
   -4,  /* (436) perfetto_macro_arg_list_ne ::= perfetto_macro_arg_list_ne COMMA ID ID */
   -1,  /* (437) perfetto_module_name ::= ID|STAR|INTERSECT */
   -3,  /* (438) perfetto_module_name ::= perfetto_module_name DOT ID|STAR|INTERSECT */
    0,  /* (439) select_body_start ::= */
    0,  /* (440) select_body_end ::= */
   -2,  /* (441) perfetto_pipe ::= BITOR GT */
   -3,  /* (442) perfetto_pipe_source ::= nm dbnm as */
   -4,  /* (443) perfetto_pipe_source ::= LP select RP as */
   -1,  /* (444) perfetto_tree_direction ::= UP */
   -1,  /* (445) perfetto_tree_direction ::= DOWN */
   -3,  /* (446) perfetto_tree_aggregate ::= expr AS nm */
   -1,  /* (447) perfetto_tree_aggregate_list ::= perfetto_tree_aggregate */
   -3,  /* (448) perfetto_tree_aggregate_list ::= perfetto_tree_aggregate_list COMMA perfetto_tree_aggregate */
   -1,  /* (449) perfetto_pipe_select_item ::= nm */
   -3,  /* (450) perfetto_pipe_select_item ::= nm AS nm */
   -3,  /* (451) perfetto_pipe_select_item ::= nm DOT nm */
   -5,  /* (452) perfetto_pipe_select_item ::= nm DOT nm AS nm */
   -1,  /* (453) perfetto_pipe_select_list ::= perfetto_pipe_select_item */
   -3,  /* (454) perfetto_pipe_select_list ::= perfetto_pipe_select_list COMMA perfetto_pipe_select_item */
   -2,  /* (455) perfetto_pipe_stage ::= SELECT perfetto_pipe_select_list */
   -4,  /* (456) perfetto_pipe_stage ::= TREE ACCUMULATE perfetto_tree_direction perfetto_tree_aggregate_list */
    0,  /* (457) perfetto_pipe_stage_list ::= */
   -3,  /* (458) perfetto_pipe_stage_list ::= perfetto_pipe_stage_list perfetto_pipe perfetto_pipe_stage */
   -1,  /* (459) perfetto_pipe_source_list ::= perfetto_pipe_source */
   -3,  /* (460) perfetto_pipe_source_list ::= perfetto_pipe_source_list COMMA perfetto_pipe_source */
   -1,  /* (461) perfetto_per_col_list ::= nm */
   -3,  /* (462) perfetto_per_col_list ::= perfetto_per_col_list COMMA nm */
    0,  /* (463) perfetto_per ::= */
   -2,  /* (464) perfetto_per ::= PER perfetto_per_col_list */
   -3,  /* (465) perfetto_pipeline ::= FROM perfetto_pipe_source perfetto_pipe_stage_list */
   -8,  /* (466) perfetto_pipeline ::= INTERVAL INTERSECTION OF LP perfetto_pipe_source_list RP perfetto_per perfetto_pipe_stage_list */
   -1,  /* (467) cmd ::= perfetto_pipeline */
   -5,  /* (468) cmd ::= PERFETTO PRAGMA nm EQ expr */
  -11,  /* (469) cmd ::= CREATE perfetto_or_replace PERFETTO TABLE nm perfetto_table_impl perfetto_table_schema AS select_body_start select select_body_end */
   -9,  /* (470) cmd ::= CREATE perfetto_or_replace PERFETTO TABLE nm perfetto_table_impl perfetto_table_schema AS perfetto_pipeline */
  -10,  /* (471) cmd ::= CREATE perfetto_or_replace PERFETTO VIEW nm perfetto_table_schema AS select_body_start select select_body_end */
  -14,  /* (472) cmd ::= CREATE perfetto_or_replace PERFETTO FUNCTION nm LP perfetto_arg_def_list RP RETURNS perfetto_return_type AS select_body_start select select_body_end */
  -13,  /* (473) cmd ::= CREATE perfetto_or_replace PERFETTO FUNCTION nm LP perfetto_arg_def_list RP RETURNS perfetto_return_type DELEGATES TO ID */
  -10,  /* (474) cmd ::= CREATE perfetto_or_replace PERFETTO INDEX nm ON nm LP perfetto_indexed_col_list RP */
    0,  /* (475) before_macro_body ::= */
   -1,  /* (476) perfetto_macro_body ::= ANY */
   -2,  /* (477) perfetto_macro_body ::= perfetto_macro_body ANY */
  -13,  /* (478) cmd ::= CREATE perfetto_or_replace PERFETTO MACRO nm LP perfetto_macro_arg_list RP RETURNS ID before_macro_body AS perfetto_macro_body */
   -4,  /* (479) cmd ::= INCLUDE PERFETTO MODULE perfetto_module_name */
   -6,  /* (480) cmd ::= DROP PERFETTO INDEX nm ON nm */
};

static void yy_accept(yyParser*);  /* Forward Declaration */

/*
** Perform a reduce action and the shift that must immediately
** follow the reduce.
**
** The yyLookahead and yyLookaheadToken parameters provide reduce actions
** access to the lookahead token (if any).  The yyLookahead will be YYNOCODE
** if the lookahead token has already been consumed.  As this procedure is
** only called from one place, optimizing compilers will in-line it, which
** means that the extra parameters have no performance impact.
*/
static YYACTIONTYPE yy_reduce(
  yyParser *yypParser,         /* The parser */
  unsigned int yyruleno,       /* Number of the rule by which to reduce */
  int yyLookahead,             /* Lookahead token, or YYNOCODE if none */
  SynqPerfettoParseTOKENTYPE yyLookaheadToken  /* Value of the lookahead token */
  SynqPerfettoParseCTX_PDECL                   /* %extra_context */
){
  int yygoto;                     /* The next state */
  YYACTIONTYPE yyact;             /* The next action */
  yyStackEntry *yymsp;            /* The top of the parser's stack */
  int yysize;                     /* Amount to pop the stack */
  SynqPerfettoParseARG_FETCH
  (void)yyLookahead;
  (void)yyLookaheadToken;
  yymsp = yypParser->yytos;
  synq_on_reduce(yypParser, yyruleno);

  switch( yyruleno ){
  /* Beginning here are the reduction cases.  A typical example
  ** follows:
  **   case 0:
  **  #line <lineno> <grammarfile>
  **     { ... }           // User supplied code
  **  #line <lineno> <thisfile>
  **     break;
  */
/********** Begin reduce actions **********************************************/
        YYMINORTYPE yylhsminor;
      case 0: /* input ::= cmdlist */
{
    pCtx->root = yymsp[0].minor.yy381;
}
        break;
      case 1: /* cmdlist ::= cmdlist ecmd */
{
    yymsp[-1].minor.yy381 = synq_pass(pCtx, yymsp[0].minor.yy381);  // Just use the last command for now
}
        break;
      case 2: /* cmdlist ::= ecmd */
      case 55: /* case_operand ::= expr */ yytestcase(yyruleno==55);
      case 176: /* expr ::= term */ yytestcase(yyruleno==176);
      case 190: /* exprlist ::= nexprlist */ yytestcase(yyruleno==190);
      case 235: /* add_column_fullname ::= fullname */ yytestcase(yyruleno==235);
      case 254: /* cmd ::= select */ yytestcase(yyruleno==254);
      case 255: /* select ::= selectnowith */ yytestcase(yyruleno==255);
      case 256: /* selectnowith ::= oneselect */ yytestcase(yyruleno==256);
      case 371: /* cmd ::= create_vtab */ yytestcase(yyruleno==371);
      case 397: /* frame_bound_s ::= frame_bound */ yytestcase(yyruleno==397);
      case 399: /* frame_bound_e ::= frame_bound */ yytestcase(yyruleno==399);
      case 410: /* filter_over ::= over_clause */ yytestcase(yyruleno==410);
{
    yylhsminor.yy381 = yymsp[0].minor.yy381;
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 3: /* ecmd ::= SEMI */
{
    yymsp[0].minor.yy381 = SYNTAQLITE_NULL_NODE;
    pCtx->stmt_completed = 1;
}
        break;
      case 4: /* ecmd ::= cmdx SEMI */
      case 261: /* sclp ::= selcollist COMMA */ yytestcase(yyruleno==261);
{
    yylhsminor.yy381 = synq_pass(pCtx, yymsp[-1].minor.yy381);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 5: /* ecmd ::= error SEMI */
{
    yymsp[-1].minor.yy381 = SYNTAQLITE_NULL_NODE;
    pCtx->root = SYNTAQLITE_NULL_NODE;
    pCtx->stmt_completed = 1;
}
        break;
      case 6: /* cmdx ::= cmd */
{
    if (pCtx->pending_explain_mode) {
        yylhsminor.yy381 = synq_parse_explain_stmt(
            pCtx, (SyntaqliteExplainMode)(pCtx->pending_explain_mode - 1), yymsp[0].minor.yy381);
        pCtx->pending_explain_mode = 0;
        // Widen the wrapper node's extents to cover the EXPLAIN /
        // EXPLAIN QUERY PLAN keyword, otherwise node_text(),
        // node_token_range() and node_expanded_text() of an explained
        // statement all drop it (e.g. "SELECT 1" for "EXPLAIN SELECT 1").
        //
        // Why this isn't automatic: the keyword lives in the `explain`
        // nonterminal, a sibling of `cmdx` in the parent rule
        // `ecmd ::= explain cmdx SEMI`. SQLite marks that rule {NEVER-REDUCE}
        // (and we mirror it: the statement is finished here, in cmdx), so the
        // reduction that would normally merge `explain`'s span into the node
        // never runs. on_reduce has already collapsed `cmd` into the shadow
        // stack top; the `explain` entry sits directly below it — fold it in
        // and re-record the widened extents on the wrapper node.
        synq_extent_fold_below_into_top(pCtx);
        synq_extent_record(pCtx, yylhsminor.yy381);
    } else {
        yylhsminor.yy381 = yymsp[0].minor.yy381;
    }
    pCtx->root = yylhsminor.yy381;
    synq_parse_list_flush(pCtx);
    pCtx->stmt_completed = 1;
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 7: /* expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist ORDER BY sortlist RP */
{
    synq_mark_as_function(pCtx, yymsp[-7].minor.yy0);
    yylhsminor.yy381 = synq_parse_aggregate_function_call(pCtx,
        synq_span(pCtx, yymsp[-7].minor.yy0),
        (SyntaqliteAggregateFunctionCallFlags){.raw = (uint8_t)(yymsp[-5].minor.yy381 & 0xFF)},
        yymsp[-4].minor.yy381,
        yymsp[-1].minor.yy381,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE);
}
  yymsp[-7].minor.yy381 = yylhsminor.yy381;
        break;
      case 8: /* expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist ORDER BY sortlist RP filter_over */
{
    SyntaqliteFilterOver *fo = AST_NODE_AS(SyntaqliteFilterOver, &pCtx->ast, yymsp[0].minor.yy381);
    synq_mark_as_function(pCtx, yymsp[-8].minor.yy0);
    yylhsminor.yy381 = synq_parse_aggregate_function_call(pCtx,
        synq_span(pCtx, yymsp[-8].minor.yy0),
        (SyntaqliteAggregateFunctionCallFlags){.raw = (uint8_t)(yymsp[-6].minor.yy381 & 0xFF)},
        yymsp[-5].minor.yy381,
        yymsp[-2].minor.yy381,
        fo->filter_expr,
        fo->over_def);
}
  yymsp[-8].minor.yy381 = yylhsminor.yy381;
        break;
      case 9: /* expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP WITHIN GROUP LP ORDER BY expr RP */
{
    synq_mark_as_function(pCtx, yymsp[-11].minor.yy0);
    yylhsminor.yy381 = synq_parse_ordered_set_function_call(pCtx,
        synq_span(pCtx, yymsp[-11].minor.yy0),
        (SyntaqliteAggregateFunctionCallFlags){.raw = (uint8_t)(yymsp[-9].minor.yy381 & 0xFF)},
        yymsp[-8].minor.yy381,
        yymsp[-1].minor.yy381,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE);
}
  yymsp[-11].minor.yy381 = yylhsminor.yy381;
        break;
      case 10: /* expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP WITHIN GROUP LP ORDER BY expr RP filter_over */
{
    SyntaqliteFilterOver *fo = AST_NODE_AS(SyntaqliteFilterOver, &pCtx->ast, yymsp[0].minor.yy381);
    synq_mark_as_function(pCtx, yymsp[-12].minor.yy0);
    yylhsminor.yy381 = synq_parse_ordered_set_function_call(pCtx,
        synq_span(pCtx, yymsp[-12].minor.yy0),
        (SyntaqliteAggregateFunctionCallFlags){.raw = (uint8_t)(yymsp[-10].minor.yy381 & 0xFF)},
        yymsp[-9].minor.yy381,
        yymsp[-2].minor.yy381,
        fo->filter_expr,
        fo->over_def);
}
  yymsp[-12].minor.yy381 = yylhsminor.yy381;
        break;
      case 11: /* expr ::= CAST LP expr AS typetoken RP */
{
    yymsp[-5].minor.yy381 = synq_parse_cast_expr(pCtx, yymsp[-3].minor.yy381, synq_span(pCtx, yymsp[-1].minor.yy0));
}
        break;
      case 12: /* typetoken ::= */
{
    yymsp[1].minor.yy0.n = 0; yymsp[1].minor.yy0.z = 0;
}
        break;
      case 13: /* typetoken ::= typename */
{
    (void)yymsp[0].minor.yy0;
}
        break;
      case 14: /* typetoken ::= typename LP signed RP */
{
    yymsp[-3].minor.yy0.n = (int)(&yymsp[0].minor.yy0.z[yymsp[0].minor.yy0.n] - yymsp[-3].minor.yy0.z);
}
        break;
      case 15: /* typetoken ::= typename LP signed COMMA signed RP */
{
    yymsp[-5].minor.yy0.n = (int)(&yymsp[0].minor.yy0.z[yymsp[0].minor.yy0.n] - yymsp[-5].minor.yy0.z);
}
        break;
      case 16: /* typename ::= ID|STRING */
      case 417: /* perfetto_arg_type ::= ID */ yytestcase(yyruleno==417);
{
    synq_mark_as_type(pCtx, yymsp[0].minor.yy0);
    yylhsminor.yy0 = yymsp[0].minor.yy0;
}
  yymsp[0].minor.yy0 = yylhsminor.yy0;
        break;
      case 17: /* typename ::= typename ID|STRING */
{
    synq_mark_as_type(pCtx, yymsp[0].minor.yy0);
    yymsp[-1].minor.yy0.n = yymsp[0].minor.yy0.n + (int)(yymsp[0].minor.yy0.z - yymsp[-1].minor.yy0.z);
}
        break;
      case 18: /* selcollist ::= sclp scanpt nm DOT STAR */
{
    uint32_t expr = synq_parse_ident_name(pCtx, synq_span(pCtx, yymsp[-2].minor.yy0));
    uint32_t col = synq_parse_result_column(pCtx, (SyntaqliteResultColumnFlags){.raw = 0x01},
                                           SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE, expr);
    yylhsminor.yy381 = synq_parse_result_column_list(pCtx, yymsp[-4].minor.yy381, col);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 19: /* expr ::= ID|INDEXED|JOIN_KW */
{
    synq_mark_as_id(pCtx, yymsp[0].minor.yy0);
    yylhsminor.yy381 = synq_parse_column_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0),
        SYNQ_NO_SPAN,
        SYNQ_NO_SPAN);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 20: /* expr ::= nm DOT nm */
{
    yylhsminor.yy381 = synq_parse_column_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0),
        synq_span_dequote(pCtx, yymsp[-2].minor.yy0),
        SYNQ_NO_SPAN);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 21: /* expr ::= nm DOT nm DOT nm */
{
    yylhsminor.yy381 = synq_parse_column_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0),
        synq_span_dequote(pCtx, yymsp[-2].minor.yy0),
        synq_span_dequote(pCtx, yymsp[-4].minor.yy0));
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 22: /* selectnowith ::= selectnowith multiselect_op oneselect */
{
    // ORDER BY / LIMIT parse inside the last arm (grammar shape) but apply
    // to the whole compound — hoist them onto the CompoundSelect node.
    // An ORDER BY on a non-last arm stays put (SQLite rejects it later).
    uint32_t orderby = SYNTAQLITE_NULL_NODE;
    uint32_t limit = SYNTAQLITE_NULL_NODE;
    SyntaqliteNode *arm = AST_NODE(&pCtx->ast, yymsp[0].minor.yy381);
    if (arm->tag == SYNTAQLITE_NODE_SELECT_STMT) {
        orderby = arm->select_stmt.orderby;
        limit = arm->select_stmt.limit_clause;
        arm->select_stmt.orderby = SYNTAQLITE_NULL_NODE;
        arm->select_stmt.limit_clause = SYNTAQLITE_NULL_NODE;
    }
    yymsp[-2].minor.yy381 = synq_parse_compound_select(pCtx, (SyntaqliteCompoundOp)yymsp[-1].minor.yy686, yymsp[-2].minor.yy381, yymsp[0].minor.yy381, orderby, limit);
}
        break;
      case 23: /* multiselect_op ::= UNION */
{ yylhsminor.yy686 = 0; (void)yymsp[0].minor.yy0; }
  yymsp[0].minor.yy686 = yylhsminor.yy686;
        break;
      case 24: /* multiselect_op ::= UNION ALL */
      case 29: /* in_op ::= NOT IN */ yytestcase(yyruleno==29);
      case 416: /* perfetto_or_replace ::= OR REPLACE */ yytestcase(yyruleno==416);
{ yymsp[-1].minor.yy686 = 1; }
        break;
      case 25: /* multiselect_op ::= EXCEPT|INTERSECT */
{
    yylhsminor.yy686 = (yymsp[0].minor.yy0.type == SYNTAQLITE_TK_INTERSECT) ? 2 : 3;
}
  yymsp[0].minor.yy686 = yylhsminor.yy686;
        break;
      case 26: /* expr ::= LP select RP */
{
    pCtx->saw_subquery = 1;
    yymsp[-2].minor.yy381 = synq_parse_subquery_expr(pCtx, yymsp[-1].minor.yy381);
}
        break;
      case 27: /* expr ::= EXISTS LP select RP */
{
    pCtx->saw_subquery = 1;
    yymsp[-3].minor.yy381 = synq_parse_exists_expr(pCtx, yymsp[-1].minor.yy381);
}
        break;
      case 28: /* in_op ::= IN */
{ yymsp[0].minor.yy686 = 0; }
        break;
      case 30: /* expr ::= expr in_op LP exprlist RP */
{
    yymsp[-4].minor.yy381 = synq_parse_in_expr(pCtx, (SyntaqliteBool)yymsp[-3].minor.yy686,
                           SYNTAQLITE_BOOL_FALSE, yymsp[-4].minor.yy381, yymsp[-1].minor.yy381);
}
        break;
      case 31: /* expr ::= expr in_op LP select RP */
{
    pCtx->saw_subquery = 1;
    // Pass the raw select node directly — InExpr's fmt block already adds
    // the surrounding parens, so wrapping in SubqueryExpr would double them.
    yymsp[-4].minor.yy381 = synq_parse_in_expr(pCtx, (SyntaqliteBool)yymsp[-3].minor.yy686,
                           SYNTAQLITE_BOOL_FALSE, yymsp[-4].minor.yy381, yymsp[-1].minor.yy381);
}
        break;
      case 32: /* expr ::= expr in_op nm dbnm paren_exprlist */
{
    SyntaqliteTextSpan table_name;
    SyntaqliteTextSpan schema;
    if (yymsp[-1].minor.yy0.z != NULL) {
        table_name = synq_span_dequote(pCtx, yymsp[-1].minor.yy0);
        schema = synq_span_dequote(pCtx, yymsp[-2].minor.yy0);
    } else {
        table_name = synq_span_dequote(pCtx, yymsp[-2].minor.yy0);
        schema = SYNQ_NO_SPAN;
    }
    uint32_t tref = synq_parse_table_ref(pCtx, table_name, schema,
                                         yymsp[0].minor.yy194.has_parens,
                                         SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE,
                                         yymsp[0].minor.yy194.args,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
    yymsp[-4].minor.yy381 = synq_parse_in_expr(pCtx, (SyntaqliteBool)yymsp[-3].minor.yy686,
                           SYNTAQLITE_BOOL_TRUE, yymsp[-4].minor.yy381, tref);
}
        break;
      case 33: /* dbnm ::= */
{ yymsp[1].minor.yy0.z = NULL; yymsp[1].minor.yy0.n = 0; }
        break;
      case 34: /* dbnm ::= DOT nm */
{ yymsp[-1].minor.yy0 = yymsp[0].minor.yy0; }
        break;
      case 35: /* paren_exprlist ::= */
{
    yymsp[1].minor.yy194.args = SYNTAQLITE_NULL_NODE;
    yymsp[1].minor.yy194.has_parens = SYNTAQLITE_BOOL_FALSE;
}
        break;
      case 36: /* paren_exprlist ::= LP exprlist RP */
{
    yymsp[-2].minor.yy194.args = synq_pass(pCtx, yymsp[-1].minor.yy381);
    yymsp[-2].minor.yy194.has_parens = SYNTAQLITE_BOOL_TRUE;
}
        break;
      case 37: /* expr ::= expr ISNULL|NOTNULL */
{
    SyntaqliteIsOp op = (yymsp[0].minor.yy0.type == SYNTAQLITE_TK_ISNULL) ? SYNTAQLITE_IS_OP_IS_NULL : SYNTAQLITE_IS_OP_NOT_NULL;
    yylhsminor.yy381 = synq_parse_is_expr(pCtx, op, yymsp[-1].minor.yy381, SYNTAQLITE_NULL_NODE);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 38: /* expr ::= expr NOT NULL */
{
    // `NOT NULL` and `NOTNULL` mean the same thing but are different text.
    yylhsminor.yy381 = synq_parse_is_expr(pCtx, SYNTAQLITE_IS_OP_NOT_NULL_SPACED, yymsp[-2].minor.yy381,
                           SYNTAQLITE_NULL_NODE);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 39: /* expr ::= expr IS expr */
{
    yylhsminor.yy381 = synq_parse_is_expr(pCtx, SYNTAQLITE_IS_OP_IS, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 40: /* expr ::= expr IS NOT expr */
{
    yylhsminor.yy381 = synq_parse_is_expr(pCtx, SYNTAQLITE_IS_OP_IS_NOT, yymsp[-3].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-3].minor.yy381 = yylhsminor.yy381;
        break;
      case 41: /* expr ::= expr IS NOT DISTINCT FROM expr */
{
    yylhsminor.yy381 = synq_parse_is_expr(pCtx, SYNTAQLITE_IS_OP_IS_NOT_DISTINCT, yymsp[-5].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-5].minor.yy381 = yylhsminor.yy381;
        break;
      case 42: /* expr ::= expr IS DISTINCT FROM expr */
{
    yylhsminor.yy381 = synq_parse_is_expr(pCtx, SYNTAQLITE_IS_OP_IS_DISTINCT, yymsp[-4].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 43: /* between_op ::= BETWEEN */
{
    yymsp[0].minor.yy381 = 0;
}
        break;
      case 44: /* between_op ::= NOT BETWEEN */
      case 215: /* nulls ::= NULLS FIRST */ yytestcase(yyruleno==215);
{
    yymsp[-1].minor.yy381 = 1;
}
        break;
      case 45: /* expr ::= expr between_op expr AND expr */
{
    yylhsminor.yy381 = synq_parse_between_expr(pCtx, (SyntaqliteBool)yymsp[-3].minor.yy381, yymsp[-4].minor.yy381, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 46: /* likeop ::= LIKE_KW|MATCH */
{
    yylhsminor.yy0 = yymsp[0].minor.yy0;
}
  yymsp[0].minor.yy0 = yylhsminor.yy0;
        break;
      case 47: /* likeop ::= NOT LIKE_KW|MATCH */
{
    yymsp[-1].minor.yy0 = yymsp[0].minor.yy0;
    yymsp[-1].minor.yy0.n |= 0x80000000;
}
        break;
      case 48: /* expr ::= expr likeop expr */
{
    SyntaqliteBool negated = (yymsp[-1].minor.yy0.n & 0x80000000) ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE;
    uint32_t len = yymsp[-1].minor.yy0.n & 0x7FFFFFFF;
    SyntaqliteLikeKeyword kw = (len == 6) ? SYNTAQLITE_LIKE_KEYWORD_REGEXP
        : (len == 5) ? SYNTAQLITE_LIKE_KEYWORD_MATCH
        : (yymsp[-1].minor.yy0.z[0] == 'g' || yymsp[-1].minor.yy0.z[0] == 'G') ? SYNTAQLITE_LIKE_KEYWORD_GLOB
        : SYNTAQLITE_LIKE_KEYWORD_LIKE;
    yylhsminor.yy381 = synq_parse_like_expr(pCtx, negated, kw, yymsp[-2].minor.yy381, yymsp[0].minor.yy381, SYNTAQLITE_NULL_NODE);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 49: /* expr ::= expr likeop expr ESCAPE expr */
{
    SyntaqliteBool negated = (yymsp[-3].minor.yy0.n & 0x80000000) ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE;
    uint32_t len = yymsp[-3].minor.yy0.n & 0x7FFFFFFF;
    SyntaqliteLikeKeyword kw = (len == 6) ? SYNTAQLITE_LIKE_KEYWORD_REGEXP
        : (len == 5) ? SYNTAQLITE_LIKE_KEYWORD_MATCH
        : (yymsp[-3].minor.yy0.z[0] == 'g' || yymsp[-3].minor.yy0.z[0] == 'G') ? SYNTAQLITE_LIKE_KEYWORD_GLOB
        : SYNTAQLITE_LIKE_KEYWORD_LIKE;
    yylhsminor.yy381 = synq_parse_like_expr(pCtx, negated, kw, yymsp[-4].minor.yy381, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 50: /* expr ::= CASE case_operand case_exprlist case_else END */
{
    yymsp[-4].minor.yy381 = synq_parse_case_expr(pCtx, yymsp[-3].minor.yy381, yymsp[-1].minor.yy381, yymsp[-2].minor.yy381);
}
        break;
      case 51: /* case_exprlist ::= case_exprlist WHEN expr THEN expr */
{
    uint32_t w = synq_parse_case_when(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_case_when_list(pCtx, yymsp[-4].minor.yy381, w);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 52: /* case_exprlist ::= WHEN expr THEN expr */
{
    uint32_t w = synq_parse_case_when(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
    yymsp[-3].minor.yy381 = synq_parse_case_when_list(pCtx, SYNTAQLITE_NULL_NODE, w);
}
        break;
      case 53: /* case_else ::= ELSE expr */
      case 173: /* returning ::= RETURNING selcollist */ yytestcase(yyruleno==173);
      case 271: /* from ::= FROM seltablist */ yytestcase(yyruleno==271);
      case 273: /* where_opt ::= WHERE expr */ yytestcase(yyruleno==273);
      case 277: /* having_opt ::= HAVING expr */ yytestcase(yyruleno==277);
      case 313: /* when_clause ::= WHEN expr */ yytestcase(yyruleno==313);
      case 349: /* key_opt ::= KEY expr */ yytestcase(yyruleno==349);
      case 352: /* vinto ::= INTO expr */ yytestcase(yyruleno==352);
      case 408: /* window_clause ::= WINDOW windowdefn_list */ yytestcase(yyruleno==408);
{
    yymsp[-1].minor.yy381 = synq_pass(pCtx, yymsp[0].minor.yy381);
}
        break;
      case 54: /* case_else ::= */
      case 56: /* case_operand ::= */ yytestcase(yyruleno==56);
      case 69: /* carglist ::= */ yytestcase(yyruleno==69);
      case 90: /* refargs ::= */ yytestcase(yyruleno==90);
      case 106: /* conslist_opt ::= */ yytestcase(yyruleno==106);
      case 131: /* eidlist_opt ::= */ yytestcase(yyruleno==131);
      case 165: /* idlist_opt ::= */ yytestcase(yyruleno==165);
      case 174: /* returning ::= */ yytestcase(yyruleno==174);
      case 191: /* exprlist ::= */ yytestcase(yyruleno==191);
      case 262: /* sclp ::= */ yytestcase(yyruleno==262);
      case 270: /* from ::= */ yytestcase(yyruleno==270);
      case 272: /* where_opt ::= */ yytestcase(yyruleno==272);
      case 274: /* groupby_opt ::= */ yytestcase(yyruleno==274);
      case 276: /* having_opt ::= */ yytestcase(yyruleno==276);
      case 278: /* orderby_opt ::= */ yytestcase(yyruleno==278);
      case 280: /* limit_opt ::= */ yytestcase(yyruleno==280);
      case 285: /* stl_prefix ::= */ yytestcase(yyruleno==285);
      case 312: /* when_clause ::= */ yytestcase(yyruleno==312);
      case 348: /* key_opt ::= */ yytestcase(yyruleno==348);
      case 353: /* vinto ::= */ yytestcase(yyruleno==353);
      case 393: /* frame_opt ::= */ yytestcase(yyruleno==393);
{
    yymsp[1].minor.yy381 = SYNTAQLITE_NULL_NODE;
}
        break;
      case 57: /* cmd ::= create_table create_table_args */
{
    // yymsp[0].minor.yy381 is either: (1) a CreateTableStmt node with columns/constraints filled in
    // or: (2) a CreateTableStmt node with as_select filled in
    // yymsp[-1].minor.yy381 has the table name/schema/temp/ifnotexists info packed as a node.
    // We need to merge yymsp[-1].minor.yy381 info into yymsp[0].minor.yy381.
    SyntaqliteNode *ct_node = AST_NODE(&pCtx->ast, yymsp[-1].minor.yy381);
    SyntaqliteNode *args_node = AST_NODE(&pCtx->ast, yymsp[0].minor.yy381);
    args_node->create_table_stmt.table_name = ct_node->create_table_stmt.table_name;
    args_node->create_table_stmt.schema = ct_node->create_table_stmt.schema;
    args_node->create_table_stmt.temporary = ct_node->create_table_stmt.temporary;
    args_node->create_table_stmt.if_not_exists = ct_node->create_table_stmt.if_not_exists;
    yylhsminor.yy381 = synq_pass(pCtx, yymsp[0].minor.yy381);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 58: /* create_table ::= createkw temp TABLE ifnotexists nm dbnm */
{
    SyntaqliteTextSpan tbl_name = yymsp[0].minor.yy0.z ? synq_span_dequote(pCtx, yymsp[0].minor.yy0) : synq_span_dequote(pCtx, yymsp[-1].minor.yy0);
    SyntaqliteTextSpan tbl_schema = yymsp[0].minor.yy0.z ? synq_span_dequote(pCtx, yymsp[-1].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-5].minor.yy381 = synq_parse_create_table_stmt(pCtx,
        tbl_name, tbl_schema, yymsp[-4].minor.yy234, (SyntaqliteBool)yymsp[-2].minor.yy686,
        (SyntaqliteCreateTableStmtFlags){.raw = 0}, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 59: /* create_table_args ::= LP columnlist conslist_opt RP table_option_set */
{
    yymsp[-4].minor.yy381 = synq_parse_create_table_stmt(pCtx,
        SYNQ_NO_SPAN, SYNQ_NO_SPAN, SYNTAQLITE_TEMPORARY_QUALIFIER_NONE, SYNTAQLITE_BOOL_FALSE,
        (SyntaqliteCreateTableStmtFlags){.raw = (uint8_t)(yymsp[0].minor.yy686 & 0xFF)}, yymsp[-3].minor.yy381, yymsp[-2].minor.yy381, SYNTAQLITE_NULL_NODE);
}
        break;
      case 60: /* create_table_args ::= AS select */
{
    yymsp[-1].minor.yy381 = synq_parse_create_table_stmt(pCtx,
        SYNQ_NO_SPAN, SYNQ_NO_SPAN, SYNTAQLITE_TEMPORARY_QUALIFIER_NONE, SYNTAQLITE_BOOL_FALSE,
        (SyntaqliteCreateTableStmtFlags){.raw = 0}, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
        break;
      case 61: /* table_option_set ::= */
      case 88: /* autoinc ::= */ yytestcase(yyruleno==88);
      case 135: /* collate ::= */ yytestcase(yyruleno==135);
      case 226: /* ifexists ::= */ yytestcase(yyruleno==226);
      case 236: /* kwcolumn_opt ::= */ yytestcase(yyruleno==236);
      case 250: /* savepoint_opt ::= */ yytestcase(yyruleno==250);
      case 310: /* foreach_clause ::= */ yytestcase(yyruleno==310);
      case 347: /* database_kw_opt ::= */ yytestcase(yyruleno==347);
      case 359: /* uniqueflag ::= */ yytestcase(yyruleno==359);
      case 360: /* ifnotexists ::= */ yytestcase(yyruleno==360);
{
    yymsp[1].minor.yy686 = 0;
}
        break;
      case 62: /* table_option_set ::= table_option */
      case 118: /* defer_subclause_opt ::= defer_subclause */ yytestcase(yyruleno==118);
{
    // passthrough
}
        break;
      case 63: /* table_option_set ::= table_option_set COMMA table_option */
{
    yylhsminor.yy686 = yymsp[-2].minor.yy686 | yymsp[0].minor.yy686;
}
  yymsp[-2].minor.yy686 = yylhsminor.yy686;
        break;
      case 64: /* table_option ::= WITHOUT nm */
{
    // WITHOUT ROWID = bit 0
    if (yymsp[0].minor.yy0.n == 5 && SYNQ_STRNCASECMP(yymsp[0].minor.yy0.z, "rowid", 5) == 0) {
        yymsp[-1].minor.yy686 = 1;
    } else {
        yymsp[-1].minor.yy686 = 0;
        pCtx->error = 1;
    }
}
        break;
      case 65: /* table_option ::= nm */
{
    // STRICT = bit 1
    if (yymsp[0].minor.yy0.n == 6 && SYNQ_STRNCASECMP(yymsp[0].minor.yy0.z, "strict", 6) == 0) {
        yylhsminor.yy686 = 2;
    } else {
        yylhsminor.yy686 = 0;
        pCtx->error = 1;
    }
}
  yymsp[0].minor.yy686 = yylhsminor.yy686;
        break;
      case 66: /* columnlist ::= columnlist COMMA columnname carglist */
{
    uint32_t col = synq_parse_column_def(pCtx, yymsp[-1].minor.yy718.name, yymsp[-1].minor.yy718.typetoken, yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_column_def_list(pCtx, yymsp[-3].minor.yy381, col);
}
  yymsp[-3].minor.yy381 = yylhsminor.yy381;
        break;
      case 67: /* columnlist ::= columnname carglist */
{
    uint32_t col = synq_parse_column_def(pCtx, yymsp[-1].minor.yy718.name, yymsp[-1].minor.yy718.typetoken, yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_column_def_list(pCtx, SYNTAQLITE_NULL_NODE, col);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 68: /* carglist ::= carglist ccons */
{
    yylhsminor.yy381 = synq_parse_column_constraint_list(pCtx, yymsp[-1].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 70: /* ccons ::= CONSTRAINT nm */
{
    SyntaqliteTextSpan name = synq_span(pCtx, yymsp[0].minor.yy0);
    yymsp[-1].minor.yy381 = synq_parse_constraint_name_declaration(pCtx, name);
}
        break;
      case 71: /* ccons ::= DEFAULT scantok term */
{
    yymsp[-2].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_DEFAULT,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        yymsp[0].minor.yy381, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 72: /* ccons ::= DEFAULT LP expr RP */
{
    yymsp[-3].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_DEFAULT,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_TRUE, SYNTAQLITE_BOOL_FALSE,
        yymsp[-1].minor.yy381, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 73: /* ccons ::= DEFAULT PLUS scantok term */
{
    uint32_t pos = synq_parse_unary_expr(pCtx, SYNTAQLITE_UNARY_OP_PLUS, yymsp[0].minor.yy381);
    yymsp[-3].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_DEFAULT,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        pos, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 74: /* ccons ::= DEFAULT MINUS scantok term */
{
    // Create a unary minus wrapping the term
    uint32_t neg = synq_parse_unary_expr(pCtx, SYNTAQLITE_UNARY_OP_MINUS, yymsp[0].minor.yy381);
    yymsp[-3].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_DEFAULT,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        neg, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 75: /* ccons ::= DEFAULT scantok ID|INDEXED */
{
    // `DEFAULT foo` is a string literal upstream (tokenExpr TK_STRING), not
    // a column reference: as an expression it would not be constant.
    uint32_t ref = synq_parse_literal(pCtx, SYNTAQLITE_LITERAL_TYPE_STRING,
                                      synq_span(pCtx, yymsp[0].minor.yy0));
    yymsp[-2].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_DEFAULT,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        ref, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 76: /* ccons ::= NULL onconf */
{
    yymsp[-1].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_NULL,
        (SyntaqliteConflictAction)yymsp[0].minor.yy686, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 77: /* ccons ::= NOT NULL onconf */
{
    yymsp[-2].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_NOT_NULL,
        (SyntaqliteConflictAction)yymsp[0].minor.yy686, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 78: /* ccons ::= PRIMARY KEY sortorder onconf autoinc */
{
    yymsp[-4].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_PRIMARY_KEY,
        (SyntaqliteConflictAction)yymsp[-1].minor.yy686, (SyntaqliteSortOrder)yymsp[-2].minor.yy381, (SyntaqliteBool)yymsp[0].minor.yy686,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 79: /* ccons ::= UNIQUE onconf */
{
    yymsp[-1].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_UNIQUE,
        (SyntaqliteConflictAction)yymsp[0].minor.yy686, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 80: /* ccons ::= CHECK LP expr RP */
{
    yymsp[-3].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_CHECK,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, yymsp[-1].minor.yy381, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 81: /* ccons ::= REFERENCES nm eidlist_opt refargs */
{
    uint32_t fk = synq_parse_foreign_key_clause(pCtx,
        synq_span(pCtx, yymsp[-2].minor.yy0), yymsp[-1].minor.yy381, yymsp[0].minor.yy381,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET);
    yymsp[-3].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_REFERENCES,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, fk);
}
        break;
      case 82: /* ccons ::= defer_subclause */
{
    yylhsminor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_DEFERRABLE,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        yymsp[0].minor.yy581.deferrable, yymsp[0].minor.yy581.initial,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 83: /* ccons ::= COLLATE ID|STRING */
{
    yymsp[-1].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_COLLATE,
        0, 0, 0,
        synq_span(pCtx, yymsp[0].minor.yy0),
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 84: /* ccons ::= GENERATED ALWAYS AS generated */
{
    yymsp[-3].minor.yy381 = yymsp[0].minor.yy381;
    if (yymsp[-3].minor.yy381 != SYNTAQLITE_NULL_NODE) {
        SyntaqliteNode *node = AST_NODE(&pCtx->ast, yymsp[-3].minor.yy381);
        node->column_constraint.generated_always = SYNTAQLITE_BOOL_TRUE;
    }
}
        break;
      case 85: /* ccons ::= AS generated */
{
    yymsp[-1].minor.yy381 = yymsp[0].minor.yy381;
    if (yymsp[-1].minor.yy381 != SYNTAQLITE_NULL_NODE && pCtx->generated_always) {
        SyntaqliteNode *node = AST_NODE(&pCtx->ast, yymsp[-1].minor.yy381);
        node->column_constraint.generated_always = SYNTAQLITE_BOOL_TRUE;
    }
}
        break;
      case 86: /* generated ::= LP expr RP */
{
    yymsp[-2].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_GENERATED,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        SYNTAQLITE_GENERATED_COLUMN_STORAGE_NONE,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, yymsp[-1].minor.yy381, SYNTAQLITE_NULL_NODE);
}
        break;
      case 87: /* generated ::= LP expr RP ID */
{
    SyntaqliteGeneratedColumnStorage storage = SYNTAQLITE_GENERATED_COLUMN_STORAGE_VIRTUAL;
    if (yymsp[0].minor.yy0.n == 6 && SYNQ_STRNCASECMP(yymsp[0].minor.yy0.z, "stored", 6) == 0) {
        storage = SYNTAQLITE_GENERATED_COLUMN_STORAGE_STORED;
    } else if (!(yymsp[0].minor.yy0.n == 7 && SYNQ_STRNCASECMP(yymsp[0].minor.yy0.z, "virtual", 7) == 0)) {
        // Quoted spellings land here too, and upstream rejects those as well.
        pCtx->error = 1;
    }
    yymsp[-3].minor.yy381 = synq_parse_column_constraint(pCtx,
        SYNTAQLITE_COLUMN_CONSTRAINT_TYPE_GENERATED,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_SORT_ORDER_NONE, SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN,
        storage,
        SYNTAQLITE_DEFERRABLE_UNSET, SYNTAQLITE_INITIAL_DEFER_MODE_UNSET,
        SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, yymsp[-2].minor.yy381, SYNTAQLITE_NULL_NODE);
}
        break;
      case 89: /* autoinc ::= AUTOINCR */
      case 237: /* kwcolumn_opt ::= COLUMNKW */ yytestcase(yyruleno==237);
      case 249: /* savepoint_opt ::= SAVEPOINT */ yytestcase(yyruleno==249);
      case 346: /* database_kw_opt ::= DATABASE */ yytestcase(yyruleno==346);
      case 358: /* uniqueflag ::= UNIQUE */ yytestcase(yyruleno==358);
{
    yymsp[0].minor.yy686 = 1;
}
        break;
      case 91: /* refargs ::= refargs refarg */
{
    yymsp[-1].minor.yy381 = synq_parse_foreign_key_option_list(pCtx, yymsp[-1].minor.yy381, yymsp[0].minor.yy381);
}
        break;
      case 92: /* refarg ::= MATCH nm */
{
    yymsp[-1].minor.yy381 = synq_parse_foreign_key_option(pCtx, SYNTAQLITE_FOREIGN_KEY_OPTION_KIND_MATCH,
        SYNTAQLITE_FOREIGN_KEY_ACTION_UNSET, synq_span(pCtx, yymsp[0].minor.yy0));
}
        break;
      case 93: /* refarg ::= ON INSERT refact */
{
    yymsp[-2].minor.yy381 = synq_parse_foreign_key_option(pCtx, SYNTAQLITE_FOREIGN_KEY_OPTION_KIND_ON_INSERT,
        (SyntaqliteForeignKeyAction)yymsp[0].minor.yy686, SYNQ_NO_SPAN);
}
        break;
      case 94: /* refarg ::= ON DELETE refact */
{
    yymsp[-2].minor.yy381 = synq_parse_foreign_key_option(pCtx, SYNTAQLITE_FOREIGN_KEY_OPTION_KIND_ON_DELETE,
        (SyntaqliteForeignKeyAction)yymsp[0].minor.yy686, SYNQ_NO_SPAN);
}
        break;
      case 95: /* refarg ::= ON UPDATE refact */
{
    yymsp[-2].minor.yy381 = synq_parse_foreign_key_option(pCtx, SYNTAQLITE_FOREIGN_KEY_OPTION_KIND_ON_UPDATE,
        (SyntaqliteForeignKeyAction)yymsp[0].minor.yy686, SYNQ_NO_SPAN);
}
        break;
      case 96: /* refact ::= SET NULL */
{
    yymsp[-1].minor.yy686 = (int)SYNTAQLITE_FOREIGN_KEY_ACTION_SET_NULL;
}
        break;
      case 97: /* refact ::= SET DEFAULT */
{
    yymsp[-1].minor.yy686 = (int)SYNTAQLITE_FOREIGN_KEY_ACTION_SET_DEFAULT;
}
        break;
      case 98: /* refact ::= CASCADE */
{
    yymsp[0].minor.yy686 = (int)SYNTAQLITE_FOREIGN_KEY_ACTION_CASCADE;
}
        break;
      case 99: /* refact ::= RESTRICT */
{
    yymsp[0].minor.yy686 = (int)SYNTAQLITE_FOREIGN_KEY_ACTION_RESTRICT;
}
        break;
      case 100: /* refact ::= NO ACTION */
{
    yymsp[-1].minor.yy686 = (int)SYNTAQLITE_FOREIGN_KEY_ACTION_NO_ACTION;
}
        break;
      case 101: /* defer_subclause ::= NOT DEFERRABLE init_deferred_pred_opt */
{
    yymsp[-2].minor.yy581.deferrable = SYNTAQLITE_DEFERRABLE_NOT_DEFERRABLE;
    yymsp[-2].minor.yy581.initial = (SyntaqliteInitialDeferMode)yymsp[0].minor.yy686;
}
        break;
      case 102: /* defer_subclause ::= DEFERRABLE init_deferred_pred_opt */
{
    yymsp[-1].minor.yy581.deferrable = SYNTAQLITE_DEFERRABLE_DEFERRABLE;
    yymsp[-1].minor.yy581.initial = (SyntaqliteInitialDeferMode)yymsp[0].minor.yy686;
}
        break;
      case 103: /* init_deferred_pred_opt ::= */
{
    yymsp[1].minor.yy686 = (int)SYNTAQLITE_INITIAL_DEFER_MODE_UNSET;
}
        break;
      case 104: /* init_deferred_pred_opt ::= INITIALLY DEFERRED */
{
    yymsp[-1].minor.yy686 = (int)SYNTAQLITE_INITIAL_DEFER_MODE_DEFERRED;
}
        break;
      case 105: /* init_deferred_pred_opt ::= INITIALLY IMMEDIATE */
{
    yymsp[-1].minor.yy686 = (int)SYNTAQLITE_INITIAL_DEFER_MODE_IMMEDIATE;
}
        break;
      case 107: /* conslist_opt ::= COMMA conslist */
{
    yymsp[-1].minor.yy381 = synq_parse_table_constraint_list(pCtx, yymsp[0].minor.yy699.list, yymsp[0].minor.yy699.group);
}
        break;
      case 108: /* conslist ::= conslist tconscomma tcons */
{
    yylhsminor.yy699.list = yymsp[-2].minor.yy699.list;
    uint32_t group = yymsp[-2].minor.yy699.group;
    if (yymsp[-1].minor.yy686) {
        yylhsminor.yy699.list = synq_parse_table_constraint_list(pCtx, yymsp[-2].minor.yy699.list, yymsp[-2].minor.yy699.group);
        group = SYNTAQLITE_NULL_NODE;
    }
    yylhsminor.yy699.group = synq_parse_list_append_from_children(pCtx,
        SYNTAQLITE_NODE_TABLE_CONSTRAINT_GROUP, group, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy699 = yylhsminor.yy699;
        break;
      case 109: /* conslist ::= tcons */
{
    yylhsminor.yy699.list = SYNTAQLITE_NULL_NODE;
    yylhsminor.yy699.group = synq_parse_list_append_from_children(pCtx,
        SYNTAQLITE_NODE_TABLE_CONSTRAINT_GROUP, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy699 = yylhsminor.yy699;
        break;
      case 110: /* tconscomma ::= COMMA */
{ yymsp[0].minor.yy686 = 1; }
        break;
      case 111: /* tconscomma ::= */
      case 415: /* perfetto_or_replace ::= */ yytestcase(yyruleno==415);
{ yymsp[1].minor.yy686 = 0; }
        break;
      case 112: /* tcons ::= CONSTRAINT nm */
{
    yymsp[-1].minor.yy381 = synq_parse_constraint_name_declaration(pCtx, synq_span(pCtx, yymsp[0].minor.yy0));
}
        break;
      case 113: /* tcons ::= PRIMARY KEY LP sortlist autoinc RP onconf */
{
    yymsp[-6].minor.yy381 = synq_parse_table_constraint(pCtx,
        SYNTAQLITE_TABLE_CONSTRAINT_TYPE_PRIMARY_KEY,
        (SyntaqliteConflictAction)yymsp[0].minor.yy686, (SyntaqliteBool)yymsp[-2].minor.yy686,
        yymsp[-3].minor.yy381, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 114: /* tcons ::= UNIQUE LP sortlist RP onconf */
{
    yymsp[-4].minor.yy381 = synq_parse_table_constraint(pCtx,
        SYNTAQLITE_TABLE_CONSTRAINT_TYPE_UNIQUE,
        (SyntaqliteConflictAction)yymsp[0].minor.yy686, SYNTAQLITE_BOOL_FALSE,
        yymsp[-2].minor.yy381, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 115: /* tcons ::= CHECK LP expr RP onconf */
{
    yymsp[-4].minor.yy381 = synq_parse_table_constraint(pCtx,
        SYNTAQLITE_TABLE_CONSTRAINT_TYPE_CHECK,
        (SyntaqliteConflictAction)yymsp[0].minor.yy686, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, yymsp[-2].minor.yy381, SYNTAQLITE_NULL_NODE);
}
        break;
      case 116: /* tcons ::= FOREIGN KEY LP eidlist RP REFERENCES nm eidlist_opt refargs defer_subclause_opt */
{
    uint32_t fk = synq_parse_foreign_key_clause(pCtx,
        synq_span(pCtx, yymsp[-3].minor.yy0), yymsp[-2].minor.yy381, yymsp[-1].minor.yy381,
        yymsp[0].minor.yy581.deferrable, yymsp[0].minor.yy581.initial);
    yymsp[-9].minor.yy381 = synq_parse_table_constraint(pCtx,
        SYNTAQLITE_TABLE_CONSTRAINT_TYPE_FOREIGN_KEY,
        SYNTAQLITE_CONFLICT_ACTION_DEFAULT, SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, yymsp[-6].minor.yy381, SYNTAQLITE_NULL_NODE, fk);
}
        break;
      case 117: /* defer_subclause_opt ::= */
{
    yymsp[1].minor.yy581.deferrable = SYNTAQLITE_DEFERRABLE_UNSET;
    yymsp[1].minor.yy581.initial = SYNTAQLITE_INITIAL_DEFER_MODE_UNSET;
}
        break;
      case 119: /* onconf ::= */
      case 146: /* orconf ::= */ yytestcase(yyruleno==146);
{
    yymsp[1].minor.yy686 = (int)SYNTAQLITE_CONFLICT_ACTION_DEFAULT;
}
        break;
      case 120: /* onconf ::= ON CONFLICT resolvetype */
{
    yymsp[-2].minor.yy686 = yymsp[0].minor.yy686;
}
        break;
      case 121: /* scantok ::= */
      case 155: /* indexed_opt ::= */ yytestcase(yyruleno==155);
      case 263: /* scanpt ::= */ yytestcase(yyruleno==263);
{
    yymsp[1].minor.yy0.z = NULL; yymsp[1].minor.yy0.n = 0;
}
        break;
      case 122: /* select ::= WITH wqlist selectnowith */
{
    yymsp[-2].minor.yy381 = synq_parse_with_clause(pCtx, 0, yymsp[-1].minor.yy381, yymsp[0].minor.yy381);
}
        break;
      case 123: /* select ::= WITH RECURSIVE wqlist selectnowith */
{
    yymsp[-3].minor.yy381 = synq_parse_with_clause(pCtx, 1, yymsp[-1].minor.yy381, yymsp[0].minor.yy381);
}
        break;
      case 124: /* wqitem ::= withnm eidlist_opt wqas LP select RP */
{
    yylhsminor.yy381 = synq_parse_cte_definition(pCtx, synq_span_dequote(pCtx, yymsp[-5].minor.yy0), (SyntaqliteMaterialized)yymsp[-3].minor.yy686, yymsp[-4].minor.yy381, yymsp[-1].minor.yy381);
}
  yymsp[-5].minor.yy381 = yylhsminor.yy381;
        break;
      case 125: /* wqlist ::= wqitem */
{
    yylhsminor.yy381 = synq_parse_cte_list(pCtx, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 126: /* wqlist ::= wqlist COMMA wqitem */
{
    yymsp[-2].minor.yy381 = synq_parse_cte_list(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
        break;
      case 127: /* withnm ::= nm */
{
    // Token passthrough - nm already produces SynqParseToken
}
        break;
      case 128: /* wqas ::= AS */
{
    yymsp[0].minor.yy686 = (int)SYNTAQLITE_MATERIALIZED_DEFAULT;
}
        break;
      case 129: /* wqas ::= AS MATERIALIZED */
{
    yymsp[-1].minor.yy686 = (int)SYNTAQLITE_MATERIALIZED_MATERIALIZED;
}
        break;
      case 130: /* wqas ::= AS NOT MATERIALIZED */
{
    yymsp[-2].minor.yy686 = (int)SYNTAQLITE_MATERIALIZED_NOT_MATERIALIZED;
}
        break;
      case 132: /* eidlist_opt ::= LP eidlist RP */
      case 166: /* idlist_opt ::= LP idlist RP */ yytestcase(yyruleno==166);
      case 324: /* trigger_cmd ::= scanpt select scanpt */ yytestcase(yyruleno==324);
{
    yymsp[-2].minor.yy381 = synq_pass(pCtx, yymsp[-1].minor.yy381);
}
        break;
      case 133: /* eidlist ::= nm collate sortorder */
{
    if (yymsp[-1].minor.yy686 || yymsp[0].minor.yy381 != SYNQ_SORTORDER_NONE) {
        pCtx->error = 1;
    }
    uint32_t col = synq_parse_column_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[-2].minor.yy0),
        SYNQ_NO_SPAN,
        SYNQ_NO_SPAN);
    yylhsminor.yy381 = synq_parse_expr_list(pCtx, SYNTAQLITE_NULL_NODE, col);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 134: /* eidlist ::= eidlist COMMA nm collate sortorder */
{
    if (yymsp[-1].minor.yy686 || yymsp[0].minor.yy381 != SYNQ_SORTORDER_NONE) {
        pCtx->error = 1;
    }
    uint32_t col = synq_parse_column_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[-2].minor.yy0),
        SYNQ_NO_SPAN,
        SYNQ_NO_SPAN);
    yymsp[-4].minor.yy381 = synq_parse_expr_list(pCtx, yymsp[-4].minor.yy381, col);
}
        break;
      case 136: /* collate ::= COLLATE ID|STRING */
      case 225: /* ifexists ::= IF EXISTS */ yytestcase(yyruleno==225);
{
    yymsp[-1].minor.yy686 = 1;
}
        break;
      case 137: /* with ::= */
{
    yymsp[1].minor.yy583.cte_list = SYNTAQLITE_NULL_NODE;
    yymsp[1].minor.yy583.is_recursive = 0;
}
        break;
      case 138: /* with ::= WITH wqlist */
{
    yymsp[-1].minor.yy583.cte_list = yymsp[0].minor.yy381;
    yymsp[-1].minor.yy583.is_recursive = 0;
}
        break;
      case 139: /* with ::= WITH RECURSIVE wqlist */
{
    yymsp[-2].minor.yy583.cte_list = yymsp[0].minor.yy381;
    yymsp[-2].minor.yy583.is_recursive = 1;
}
        break;
      case 140: /* cmd ::= with DELETE FROM xfullname indexed_opt where_opt_ret orderby_opt limit_opt */
{
    if (yymsp[-1].minor.yy381 != SYNTAQLITE_NULL_NODE || yymsp[0].minor.yy381 != SYNTAQLITE_NULL_NODE) {
        pCtx->saw_update_delete_limit = 1;
        if (!SYNQ_HAS_CFLAG(pCtx->env, SYNQ_CFLAG_IDX_ENABLE_UPDATE_DELETE_LIMIT)) {
            pCtx->error = 1;
        }
    }
    SyntaqliteIndexHint ih = (yymsp[-3].minor.yy0.z != NULL) ? SYNTAQLITE_INDEX_HINT_INDEXED
                           : (yymsp[-3].minor.yy0.n == 1)    ? SYNTAQLITE_INDEX_HINT_NOT_INDEXED
                           :                 SYNTAQLITE_INDEX_HINT_DEFAULT;
    yylhsminor.yy381 = synq_parse_delete_stmt(pCtx,
        yymsp[-7].minor.yy583.cte_list,
        yymsp[-7].minor.yy583.is_recursive ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-4].minor.yy381, ih, synq_span(pCtx, yymsp[-3].minor.yy0), yymsp[-2].minor.yy423.where_expr, yymsp[-1].minor.yy381, yymsp[0].minor.yy381, yymsp[-2].minor.yy423.returning);
}
  yymsp[-7].minor.yy381 = yylhsminor.yy381;
        break;
      case 141: /* cmd ::= with UPDATE orconf xfullname indexed_opt SET setlist from where_opt_ret orderby_opt limit_opt */
{
    if (yymsp[-1].minor.yy381 != SYNTAQLITE_NULL_NODE || yymsp[0].minor.yy381 != SYNTAQLITE_NULL_NODE) {
        pCtx->saw_update_delete_limit = 1;
        if (!SYNQ_HAS_CFLAG(pCtx->env, SYNQ_CFLAG_IDX_ENABLE_UPDATE_DELETE_LIMIT)) {
            pCtx->error = 1;
        }
    }
    SyntaqliteIndexHint ih = (yymsp[-6].minor.yy0.z != NULL) ? SYNTAQLITE_INDEX_HINT_INDEXED
                           : (yymsp[-6].minor.yy0.n == 1)    ? SYNTAQLITE_INDEX_HINT_NOT_INDEXED
                           :                 SYNTAQLITE_INDEX_HINT_DEFAULT;
    yylhsminor.yy381 = synq_parse_update_stmt(pCtx,
        yymsp[-10].minor.yy583.cte_list,
        yymsp[-10].minor.yy583.is_recursive ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        (SyntaqliteConflictAction)yymsp[-8].minor.yy686, yymsp[-7].minor.yy381, ih, synq_span(pCtx, yymsp[-6].minor.yy0), yymsp[-4].minor.yy381, yymsp[-3].minor.yy381, yymsp[-2].minor.yy423.where_expr, yymsp[-1].minor.yy381, yymsp[0].minor.yy381, yymsp[-2].minor.yy423.returning);
}
  yymsp[-10].minor.yy381 = yylhsminor.yy381;
        break;
      case 142: /* cmd ::= with insert_cmd INTO xfullname idlist_opt select upsert */
{
    yylhsminor.yy381 = synq_parse_insert_stmt(pCtx,
        yymsp[-6].minor.yy583.cte_list,
        yymsp[-6].minor.yy583.is_recursive ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-5].minor.yy90.keyword, yymsp[-5].minor.yy90.conflict_action, yymsp[-3].minor.yy381, yymsp[-2].minor.yy381, yymsp[-1].minor.yy381, yymsp[0].minor.yy54.clauses, yymsp[0].minor.yy54.returning);
}
  yymsp[-6].minor.yy381 = yylhsminor.yy381;
        break;
      case 143: /* cmd ::= with insert_cmd INTO xfullname idlist_opt DEFAULT VALUES returning */
{
    yylhsminor.yy381 = synq_parse_insert_stmt(pCtx,
        yymsp[-7].minor.yy583.cte_list,
        yymsp[-7].minor.yy583.is_recursive ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-6].minor.yy90.keyword, yymsp[-6].minor.yy90.conflict_action, yymsp[-4].minor.yy381, yymsp[-3].minor.yy381, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
  yymsp[-7].minor.yy381 = yylhsminor.yy381;
        break;
      case 144: /* insert_cmd ::= INSERT orconf */
{
    yymsp[-1].minor.yy90.keyword = SYNTAQLITE_INSERT_KEYWORD_INSERT;
    yymsp[-1].minor.yy90.conflict_action = (SyntaqliteConflictAction)yymsp[0].minor.yy686;
}
        break;
      case 145: /* insert_cmd ::= REPLACE */
{
    yymsp[0].minor.yy90.keyword = SYNTAQLITE_INSERT_KEYWORD_REPLACE;
    yymsp[0].minor.yy90.conflict_action = SYNTAQLITE_CONFLICT_ACTION_REPLACE;
}
        break;
      case 147: /* orconf ::= OR resolvetype */
      case 404: /* frame_exclude_opt ::= EXCLUDE frame_exclude */ yytestcase(yyruleno==404);
{
    yymsp[-1].minor.yy686 = yymsp[0].minor.yy686;
}
        break;
      case 148: /* resolvetype ::= raisetype */
{
    // raisetype: ROLLBACK=1, ABORT=2, FAIL=3 (SynqRaiseType enum values)
    // ConflictAction: ROLLBACK=1, ABORT=2, FAIL=3 (same values, direct passthrough)
    yylhsminor.yy686 = yymsp[0].minor.yy686;
}
  yymsp[0].minor.yy686 = yylhsminor.yy686;
        break;
      case 149: /* resolvetype ::= IGNORE */
{
    yymsp[0].minor.yy686 = (int)SYNTAQLITE_CONFLICT_ACTION_IGNORE;
}
        break;
      case 150: /* resolvetype ::= REPLACE */
{
    yymsp[0].minor.yy686 = (int)SYNTAQLITE_CONFLICT_ACTION_REPLACE;
}
        break;
      case 151: /* xfullname ::= nm */
{
    yylhsminor.yy381 = synq_parse_table_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0), SYNQ_NO_SPAN,
        SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_NULL_NODE,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 152: /* xfullname ::= nm DOT nm */
{
    yylhsminor.yy381 = synq_parse_table_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0), synq_span_dequote(pCtx, yymsp[-2].minor.yy0),
        SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_NULL_NODE,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 153: /* xfullname ::= nm DOT nm AS nm */
{
    uint32_t alias = synq_parse_ident_name(pCtx, synq_span_dequote(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy381 = synq_parse_table_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[-2].minor.yy0), synq_span_dequote(pCtx, yymsp[-4].minor.yy0),
        SYNTAQLITE_BOOL_FALSE,
        alias, SYNTAQLITE_BOOL_TRUE, SYNTAQLITE_NULL_NODE,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 154: /* xfullname ::= nm AS nm */
{
    uint32_t alias = synq_parse_ident_name(pCtx, synq_span_dequote(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy381 = synq_parse_table_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[-2].minor.yy0), SYNQ_NO_SPAN,
        SYNTAQLITE_BOOL_FALSE,
        alias, SYNTAQLITE_BOOL_TRUE, SYNTAQLITE_NULL_NODE,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 156: /* indexed_opt ::= indexed_by */
      case 316: /* trnm ::= nm */ yytestcase(yyruleno==316);
      case 330: /* nmnum ::= plus_num */ yytestcase(yyruleno==330);
      case 331: /* nmnum ::= nm */ yytestcase(yyruleno==331);
      case 332: /* nmnum ::= ON */ yytestcase(yyruleno==332);
      case 333: /* nmnum ::= DELETE */ yytestcase(yyruleno==333);
      case 334: /* nmnum ::= DEFAULT */ yytestcase(yyruleno==334);
      case 336: /* plus_num ::= INTEGER|FLOAT */ yytestcase(yyruleno==336);
      case 338: /* signed ::= plus_num */ yytestcase(yyruleno==338);
      case 339: /* signed ::= minus_num */ yytestcase(yyruleno==339);
      case 363: /* createkw ::= CREATE */ yytestcase(yyruleno==363);
{
    // Token passthrough
}
        break;
      case 157: /* where_opt_ret ::= */
{
    yymsp[1].minor.yy423.where_expr = SYNTAQLITE_NULL_NODE;
    yymsp[1].minor.yy423.returning = SYNTAQLITE_NULL_NODE;
}
        break;
      case 158: /* where_opt_ret ::= WHERE expr */
{
    yymsp[-1].minor.yy423.where_expr = yymsp[0].minor.yy381;
    yymsp[-1].minor.yy423.returning = SYNTAQLITE_NULL_NODE;
}
        break;
      case 159: /* where_opt_ret ::= RETURNING selcollist */
{
    yymsp[-1].minor.yy423.where_expr = SYNTAQLITE_NULL_NODE;
    yymsp[-1].minor.yy423.returning = yymsp[0].minor.yy381;
}
        break;
      case 160: /* where_opt_ret ::= WHERE expr RETURNING selcollist */
{
    yymsp[-3].minor.yy423.where_expr = yymsp[-2].minor.yy381;
    yymsp[-3].minor.yy423.returning = yymsp[0].minor.yy381;
}
        break;
      case 161: /* setlist ::= setlist COMMA nm EQ expr */
{
    uint32_t clause = synq_parse_set_clause(pCtx,
        synq_span(pCtx, yymsp[-2].minor.yy0), SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_set_clause_list(pCtx, yymsp[-4].minor.yy381, clause);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 162: /* setlist ::= setlist COMMA LP idlist RP EQ expr */
{
    uint32_t clause = synq_parse_set_clause(pCtx,
        SYNQ_NO_SPAN, yymsp[-3].minor.yy381, yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_set_clause_list(pCtx, yymsp[-6].minor.yy381, clause);
}
  yymsp[-6].minor.yy381 = yylhsminor.yy381;
        break;
      case 163: /* setlist ::= nm EQ expr */
{
    uint32_t clause = synq_parse_set_clause(pCtx,
        synq_span(pCtx, yymsp[-2].minor.yy0), SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_set_clause_list(pCtx, SYNTAQLITE_NULL_NODE, clause);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 164: /* setlist ::= LP idlist RP EQ expr */
{
    uint32_t clause = synq_parse_set_clause(pCtx,
        SYNQ_NO_SPAN, yymsp[-3].minor.yy381, yymsp[0].minor.yy381);
    yymsp[-4].minor.yy381 = synq_parse_set_clause_list(pCtx, SYNTAQLITE_NULL_NODE, clause);
}
        break;
      case 167: /* upsert ::= */
{
    yymsp[1].minor.yy54.clauses = SYNTAQLITE_NULL_NODE;
    yymsp[1].minor.yy54.returning = SYNTAQLITE_NULL_NODE;
}
        break;
      case 168: /* upsert ::= RETURNING selcollist */
{
    yymsp[-1].minor.yy54.clauses = SYNTAQLITE_NULL_NODE;
    yymsp[-1].minor.yy54.returning = yymsp[0].minor.yy381;
}
        break;
      case 169: /* upsert ::= ON CONFLICT LP sortlist RP where_opt DO UPDATE SET setlist where_opt upsert */
{
    uint32_t clause = synq_parse_upsert_clause(pCtx, yymsp[-8].minor.yy381, yymsp[-6].minor.yy381, (SyntaqliteUpsertAction)SYNTAQLITE_UPSERT_ACTION_UPDATE, yymsp[-2].minor.yy381, yymsp[-1].minor.yy381);
    yymsp[-11].minor.yy54.clauses = synq_parse_upsert_clause_list(pCtx, yymsp[0].minor.yy54.clauses, clause);
    yymsp[-11].minor.yy54.returning = yymsp[0].minor.yy54.returning;
}
        break;
      case 170: /* upsert ::= ON CONFLICT LP sortlist RP where_opt DO NOTHING upsert */
{
    uint32_t clause = synq_parse_upsert_clause(pCtx, yymsp[-5].minor.yy381, yymsp[-3].minor.yy381, (SyntaqliteUpsertAction)SYNTAQLITE_UPSERT_ACTION_NOTHING, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
    yymsp[-8].minor.yy54.clauses = synq_parse_upsert_clause_list(pCtx, yymsp[0].minor.yy54.clauses, clause);
    yymsp[-8].minor.yy54.returning = yymsp[0].minor.yy54.returning;
}
        break;
      case 171: /* upsert ::= ON CONFLICT DO NOTHING returning */
{
    uint32_t clause = synq_parse_upsert_clause(pCtx, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, (SyntaqliteUpsertAction)SYNTAQLITE_UPSERT_ACTION_NOTHING, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
    yymsp[-4].minor.yy54.clauses = synq_parse_upsert_clause_list(pCtx, SYNTAQLITE_NULL_NODE, clause);
    yymsp[-4].minor.yy54.returning = yymsp[0].minor.yy381;
}
        break;
      case 172: /* upsert ::= ON CONFLICT DO UPDATE SET setlist where_opt returning */
{
    uint32_t clause = synq_parse_upsert_clause(pCtx, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, (SyntaqliteUpsertAction)SYNTAQLITE_UPSERT_ACTION_UPDATE, yymsp[-2].minor.yy381, yymsp[-1].minor.yy381);
    yymsp[-7].minor.yy54.clauses = synq_parse_upsert_clause_list(pCtx, SYNTAQLITE_NULL_NODE, clause);
    yymsp[-7].minor.yy54.returning = yymsp[0].minor.yy381;
}
        break;
      case 175: /* expr ::= error */
      case 202: /* nmorerr ::= error */ yytestcase(yyruleno==202);
{
    yymsp[0].minor.yy381 = synq_parse_error(pCtx, synq_error_span(pCtx));
}
        break;
      case 177: /* expr ::= LP expr RP */
{
    yymsp[-2].minor.yy381 = synq_parse_paren_expr(pCtx, yymsp[-1].minor.yy381);
}
        break;
      case 178: /* expr ::= expr PLUS|MINUS expr */
{
    SyntaqliteBinaryOp op = (yymsp[-1].minor.yy0.type == SYNTAQLITE_TK_PLUS) ? SYNTAQLITE_BINARY_OP_PLUS : SYNTAQLITE_BINARY_OP_MINUS;
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, op, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 179: /* expr ::= expr STAR|SLASH|REM expr */
{
    SyntaqliteBinaryOp op;
    switch (yymsp[-1].minor.yy0.type) {
        case SYNTAQLITE_TK_STAR:  op = SYNTAQLITE_BINARY_OP_STAR; break;
        case SYNTAQLITE_TK_SLASH: op = SYNTAQLITE_BINARY_OP_SLASH; break;
        default:       op = SYNTAQLITE_BINARY_OP_REM; break;
    }
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, op, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 180: /* expr ::= expr LT|GT|GE|LE expr */
{
    SyntaqliteBinaryOp op;
    switch (yymsp[-1].minor.yy0.type) {
        case SYNTAQLITE_TK_LT: op = SYNTAQLITE_BINARY_OP_LT; break;
        case SYNTAQLITE_TK_GT: op = SYNTAQLITE_BINARY_OP_GT; break;
        case SYNTAQLITE_TK_LE: op = SYNTAQLITE_BINARY_OP_LE; break;
        default:    op = SYNTAQLITE_BINARY_OP_GE; break;
    }
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, op, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 181: /* expr ::= expr EQ|NE expr */
{
    SyntaqliteBinaryOp op;
    if (yymsp[-1].minor.yy0.type == SYNTAQLITE_TK_EQ) {
        // `==` and `=` share one token type but are different text.
        op = (yymsp[-1].minor.yy0.n == 2) ? SYNTAQLITE_BINARY_OP_EQ_DOUBLE : SYNTAQLITE_BINARY_OP_EQ;
    } else {
        // `<>` and `!=` share one token type but are different text.
        op = (yymsp[-1].minor.yy0.n == 2 && yymsp[-1].minor.yy0.z[0] == '<') ? SYNTAQLITE_BINARY_OP_NE_ANGLE
                                           : SYNTAQLITE_BINARY_OP_NE;
    }
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, op, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 182: /* expr ::= expr AND expr */
{
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, SYNTAQLITE_BINARY_OP_AND, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 183: /* expr ::= expr OR expr */
{
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, SYNTAQLITE_BINARY_OP_OR, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 184: /* expr ::= expr BITAND|BITOR|LSHIFT|RSHIFT expr */
{
    SyntaqliteBinaryOp op;
    switch (yymsp[-1].minor.yy0.type) {
        case SYNTAQLITE_TK_BITAND: op = SYNTAQLITE_BINARY_OP_BIT_AND; break;
        case SYNTAQLITE_TK_BITOR:  op = SYNTAQLITE_BINARY_OP_BIT_OR; break;
        case SYNTAQLITE_TK_LSHIFT: op = SYNTAQLITE_BINARY_OP_LSHIFT; break;
        default:        op = SYNTAQLITE_BINARY_OP_RSHIFT; break;
    }
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, op, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 185: /* expr ::= expr CONCAT expr */
{
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, SYNTAQLITE_BINARY_OP_CONCAT, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 186: /* expr ::= expr PTR expr */
{
    SyntaqliteBinaryOp op = (yymsp[-1].minor.yy0.n == 3) ? SYNTAQLITE_BINARY_OP_PTR2 : SYNTAQLITE_BINARY_OP_PTR;
    yylhsminor.yy381 = synq_parse_binary_expr(pCtx, op, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 187: /* expr ::= PLUS|MINUS expr */
{
    SyntaqliteUnaryOp op = (yymsp[-1].minor.yy0.type == SYNTAQLITE_TK_MINUS) ? SYNTAQLITE_UNARY_OP_MINUS : SYNTAQLITE_UNARY_OP_PLUS;
    yylhsminor.yy381 = synq_parse_unary_expr(pCtx, op, yymsp[0].minor.yy381);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 188: /* expr ::= BITNOT expr */
{
    yymsp[-1].minor.yy381 = synq_parse_unary_expr(pCtx, SYNTAQLITE_UNARY_OP_BIT_NOT, yymsp[0].minor.yy381);
}
        break;
      case 189: /* expr ::= NOT expr */
{
    yymsp[-1].minor.yy381 = synq_parse_unary_expr(pCtx, SYNTAQLITE_UNARY_OP_NOT, yymsp[0].minor.yy381);
}
        break;
      case 192: /* nexprlist ::= nexprlist COMMA expr */
{
    yylhsminor.yy381 = synq_parse_expr_list(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 193: /* nexprlist ::= expr */
{
    yylhsminor.yy381 = synq_parse_expr_list(pCtx, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 194: /* expr ::= LP nexprlist COMMA expr RP */
{
    yymsp[-4].minor.yy381 = synq_parse_row_value(pCtx, synq_parse_expr_list(pCtx, yymsp[-3].minor.yy381, yymsp[-1].minor.yy381));
}
        break;
      case 195: /* expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP */
{
    synq_mark_as_function(pCtx, yymsp[-4].minor.yy0);
    yylhsminor.yy381 = synq_parse_function_call(pCtx,
        synq_span(pCtx, yymsp[-4].minor.yy0),
        (SyntaqliteFunctionCallFlags){.raw = (uint8_t)(yymsp[-2].minor.yy381 & 0xFF)},
        yymsp[-1].minor.yy381,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 196: /* expr ::= ID|INDEXED|JOIN_KW LP STAR RP */
{
    synq_mark_as_function(pCtx, yymsp[-3].minor.yy0);
    yylhsminor.yy381 = synq_parse_function_call(pCtx,
        synq_span(pCtx, yymsp[-3].minor.yy0),
        (SyntaqliteFunctionCallFlags){.raw = 0x02},
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE);
}
  yymsp[-3].minor.yy381 = yylhsminor.yy381;
        break;
      case 197: /* expr ::= ID|INDEXED|JOIN_KW LP distinct exprlist RP filter_over */
{
    SyntaqliteFilterOver *fo = AST_NODE_AS(SyntaqliteFilterOver, &pCtx->ast, yymsp[0].minor.yy381);
    synq_mark_as_function(pCtx, yymsp[-5].minor.yy0);
    yylhsminor.yy381 = synq_parse_function_call(pCtx,
        synq_span(pCtx, yymsp[-5].minor.yy0),
        (SyntaqliteFunctionCallFlags){.raw = (uint8_t)(yymsp[-3].minor.yy381 & 0xFF)},
        yymsp[-2].minor.yy381,
        fo->filter_expr,
        fo->over_def);
}
  yymsp[-5].minor.yy381 = yylhsminor.yy381;
        break;
      case 198: /* expr ::= ID|INDEXED|JOIN_KW LP STAR RP filter_over */
{
    SyntaqliteFilterOver *fo = AST_NODE_AS(SyntaqliteFilterOver, &pCtx->ast, yymsp[0].minor.yy381);
    synq_mark_as_function(pCtx, yymsp[-4].minor.yy0);
    yylhsminor.yy381 = synq_parse_function_call(pCtx,
        synq_span(pCtx, yymsp[-4].minor.yy0),
        (SyntaqliteFunctionCallFlags){.raw = 0x02},
        SYNTAQLITE_NULL_NODE,
        fo->filter_expr,
        fo->over_def);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 199: /* nm ::= ID|INDEXED|JOIN_KW */
      case 200: /* nm ::= STRING */ yytestcase(yyruleno==200);
{
    synq_mark_as_id(pCtx, yymsp[0].minor.yy0);
    yylhsminor.yy0 = yymsp[0].minor.yy0;
}
  yymsp[0].minor.yy0 = yylhsminor.yy0;
        break;
      case 201: /* nmorerr ::= nm */
{
    yylhsminor.yy381 = synq_parse_ident_name(pCtx, synq_span_dequote(pCtx, yymsp[0].minor.yy0));
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 203: /* term ::= INTEGER */
{
    yylhsminor.yy381 = synq_parse_literal(pCtx, SYNTAQLITE_LITERAL_TYPE_INTEGER, synq_span(pCtx, yymsp[0].minor.yy0));
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 204: /* term ::= STRING */
{
    yylhsminor.yy381 = synq_parse_literal(pCtx, SYNTAQLITE_LITERAL_TYPE_STRING, synq_span(pCtx, yymsp[0].minor.yy0));
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 205: /* term ::= NULL|FLOAT|BLOB */
{
    SyntaqliteLiteralType lit_type;
    switch (yymsp[0].minor.yy0.type) {
        case SYNTAQLITE_TK_NULL:  lit_type = SYNTAQLITE_LITERAL_TYPE_NULL; break;
        case SYNTAQLITE_TK_FLOAT: lit_type = SYNTAQLITE_LITERAL_TYPE_FLOAT; break;
        case SYNTAQLITE_TK_BLOB:  lit_type = SYNTAQLITE_LITERAL_TYPE_BLOB; break;
        default:       lit_type = SYNTAQLITE_LITERAL_TYPE_NULL; break;
    }
    yylhsminor.yy381 = synq_parse_literal(pCtx, lit_type, synq_span(pCtx, yymsp[0].minor.yy0));
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 206: /* term ::= QNUMBER */
{
    if (!synq_qnumber_is_valid(yymsp[0].minor.yy0.z, yymsp[0].minor.yy0.n)) {
        pCtx->error = 1;
    }
    yylhsminor.yy381 = synq_parse_literal(pCtx, SYNTAQLITE_LITERAL_TYPE_QNUMBER, synq_span(pCtx, yymsp[0].minor.yy0));
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 207: /* term ::= CTIME_KW */
{
    yylhsminor.yy381 = synq_parse_literal(pCtx, SYNTAQLITE_LITERAL_TYPE_CURRENT, synq_span(pCtx, yymsp[0].minor.yy0));
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 208: /* expr ::= VARIABLE */
{
    // `#N` names a VM register and is only legal in a nested parse, which we
    // never do.
    if (yymsp[0].minor.yy0.n >= 2 && yymsp[0].minor.yy0.z[0] == '#' && yymsp[0].minor.yy0.z[1] >= '0' && yymsp[0].minor.yy0.z[1] <= '9') {
        pCtx->error = 1;
    }
    yylhsminor.yy381 = synq_parse_variable(pCtx, synq_span(pCtx, yymsp[0].minor.yy0));
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 209: /* expr ::= expr COLLATE ID|STRING */
{
    yylhsminor.yy381 = synq_parse_collate_expr(pCtx, yymsp[-2].minor.yy381, synq_span(pCtx, yymsp[0].minor.yy0));
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 210: /* sortlist ::= sortlist COMMA expr sortorder nulls */
{
    uint32_t term = synq_parse_ordering_term(pCtx, yymsp[-2].minor.yy381, (SyntaqliteSortOrder)yymsp[-1].minor.yy381, (SyntaqliteNullsOrder)yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_order_by_list(pCtx, yymsp[-4].minor.yy381, term);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 211: /* sortlist ::= expr sortorder nulls */
{
    uint32_t term = synq_parse_ordering_term(pCtx, yymsp[-2].minor.yy381, (SyntaqliteSortOrder)yymsp[-1].minor.yy381, (SyntaqliteNullsOrder)yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_order_by_list(pCtx, SYNTAQLITE_NULL_NODE, term);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 212: /* sortorder ::= ASC */
      case 267: /* distinct ::= DISTINCT */ yytestcase(yyruleno==267);
{
    yymsp[0].minor.yy381 = 1;
}
        break;
      case 213: /* sortorder ::= DESC */
{
    yymsp[0].minor.yy381 = 2;
}
        break;
      case 214: /* sortorder ::= */
{
    yymsp[1].minor.yy381 = SYNQ_SORTORDER_NONE;
}
        break;
      case 216: /* nulls ::= NULLS LAST */
{
    yymsp[-1].minor.yy381 = 2;
}
        break;
      case 217: /* nulls ::= */
      case 269: /* distinct ::= */ yytestcase(yyruleno==269);
{
    yymsp[1].minor.yy381 = 0;
}
        break;
      case 218: /* expr ::= RAISE LP IGNORE RP */
{
    yymsp[-3].minor.yy381 = synq_parse_raise_expr(pCtx, SYNTAQLITE_RAISE_TYPE_IGNORE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 219: /* expr ::= RAISE LP raisetype COMMA expr RP */
{
    yymsp[-5].minor.yy381 = synq_parse_raise_expr(pCtx, (SyntaqliteRaiseType)yymsp[-3].minor.yy686, yymsp[-1].minor.yy381);
}
        break;
      case 220: /* raisetype ::= ROLLBACK */
{ yymsp[0].minor.yy686 = SYNTAQLITE_RAISE_TYPE_ROLLBACK; }
        break;
      case 221: /* raisetype ::= ABORT */
{ yymsp[0].minor.yy686 = SYNTAQLITE_RAISE_TYPE_ABORT; }
        break;
      case 222: /* raisetype ::= FAIL */
{ yymsp[0].minor.yy686 = SYNTAQLITE_RAISE_TYPE_FAIL; }
        break;
      case 223: /* fullname ::= nmorerr */
{
    yylhsminor.yy381 = synq_parse_qualified_name(pCtx,
        yymsp[0].minor.yy381,
        SYNTAQLITE_NULL_NODE);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 224: /* fullname ::= nmorerr DOT nmorerr */
{
    yylhsminor.yy381 = synq_parse_qualified_name(pCtx,
        yymsp[0].minor.yy381,
        yymsp[-2].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 227: /* cmd ::= DROP TABLE ifexists fullname */
{
    yymsp[-3].minor.yy381 = synq_parse_drop_stmt(pCtx, SYNTAQLITE_DROP_OBJECT_TYPE_TABLE, (SyntaqliteBool)yymsp[-1].minor.yy686, yymsp[0].minor.yy381);
}
        break;
      case 228: /* cmd ::= DROP VIEW ifexists fullname */
{
    yymsp[-3].minor.yy381 = synq_parse_drop_stmt(pCtx, SYNTAQLITE_DROP_OBJECT_TYPE_VIEW, (SyntaqliteBool)yymsp[-1].minor.yy686, yymsp[0].minor.yy381);
}
        break;
      case 229: /* cmd ::= DROP INDEX ifexists fullname */
{
    yymsp[-3].minor.yy381 = synq_parse_drop_stmt(pCtx, SYNTAQLITE_DROP_OBJECT_TYPE_INDEX, (SyntaqliteBool)yymsp[-1].minor.yy686, yymsp[0].minor.yy381);
}
        break;
      case 230: /* cmd ::= DROP TRIGGER ifexists fullname */
{
    yymsp[-3].minor.yy381 = synq_parse_drop_stmt(pCtx, SYNTAQLITE_DROP_OBJECT_TYPE_TRIGGER, (SyntaqliteBool)yymsp[-1].minor.yy686, yymsp[0].minor.yy381);
}
        break;
      case 231: /* cmd ::= ALTER TABLE fullname RENAME TO nmorerr */
{
    yymsp[-5].minor.yy381 = synq_parse_alter_table_stmt(pCtx,
        SYNTAQLITE_ALTER_OP_RENAME_TABLE, SYNTAQLITE_BOOL_FALSE, yymsp[-3].minor.yy381,
        yymsp[0].minor.yy381,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE);
}
        break;
      case 232: /* cmd ::= ALTER TABLE fullname RENAME kwcolumn_opt nmorerr TO nmorerr */
{
    yymsp[-7].minor.yy381 = synq_parse_alter_table_stmt(pCtx,
        SYNTAQLITE_ALTER_OP_RENAME_COLUMN,
        yymsp[-3].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE, yymsp[-5].minor.yy381,
        yymsp[0].minor.yy381,
        yymsp[-2].minor.yy381,
        SYNTAQLITE_NULL_NODE);
}
        break;
      case 233: /* cmd ::= ALTER TABLE fullname DROP kwcolumn_opt nmorerr */
{
    yymsp[-5].minor.yy381 = synq_parse_alter_table_stmt(pCtx,
        SYNTAQLITE_ALTER_OP_DROP_COLUMN,
        yymsp[-1].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE, yymsp[-3].minor.yy381,
        SYNTAQLITE_NULL_NODE,
        yymsp[0].minor.yy381,
        SYNTAQLITE_NULL_NODE);
}
        break;
      case 234: /* cmd ::= ALTER TABLE add_column_fullname ADD kwcolumn_opt columnname carglist */
{
    uint32_t col = synq_parse_column_def(pCtx, yymsp[-1].minor.yy718.name, yymsp[-1].minor.yy718.typetoken, yymsp[0].minor.yy381);
    yymsp[-6].minor.yy381 = synq_parse_alter_table_stmt(pCtx,
        SYNTAQLITE_ALTER_OP_ADD_COLUMN,
        yymsp[-2].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE, yymsp[-4].minor.yy381,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE,
        col);
}
        break;
      case 238: /* columnname ::= nmorerr typetoken */
{
    yylhsminor.yy718.name = yymsp[-1].minor.yy381;
    pCtx->generated_always = synq_trim_generated_always(&yymsp[0].minor.yy0);
    yylhsminor.yy718.typetoken = (yymsp[0].minor.yy0.z && yymsp[0].minor.yy0.n) ? synq_span(pCtx, yymsp[0].minor.yy0) : SYNQ_NO_SPAN;
}
  yymsp[-1].minor.yy718 = yylhsminor.yy718;
        break;
      case 239: /* cmd ::= BEGIN transtype trans_opt */
{
    yymsp[-2].minor.yy381 = synq_parse_transaction_stmt(pCtx,
        SYNTAQLITE_TRANSACTION_OP_BEGIN,
        (SyntaqliteTransactionType)yymsp[-1].minor.yy686,
        yymsp[0].minor.yy482.has_transaction ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[0].minor.yy482.name.z ? synq_span(pCtx, yymsp[0].minor.yy482.name) : SYNQ_NO_SPAN);
}
        break;
      case 240: /* cmd ::= COMMIT|END trans_opt */
{
    // END and COMMIT mean the same thing to SQLite but are different text.
    yylhsminor.yy381 = synq_parse_transaction_stmt(pCtx,
        yymsp[-1].minor.yy0.type == SYNTAQLITE_TK_END ? SYNTAQLITE_TRANSACTION_OP_END
                                    : SYNTAQLITE_TRANSACTION_OP_COMMIT,
        SYNTAQLITE_TRANSACTION_TYPE_NONE,
        yymsp[0].minor.yy482.has_transaction ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[0].minor.yy482.name.z ? synq_span(pCtx, yymsp[0].minor.yy482.name) : SYNQ_NO_SPAN);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 241: /* cmd ::= ROLLBACK trans_opt */
{
    yymsp[-1].minor.yy381 = synq_parse_transaction_stmt(pCtx,
        SYNTAQLITE_TRANSACTION_OP_ROLLBACK,
        SYNTAQLITE_TRANSACTION_TYPE_NONE,
        yymsp[0].minor.yy482.has_transaction ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[0].minor.yy482.name.z ? synq_span(pCtx, yymsp[0].minor.yy482.name) : SYNQ_NO_SPAN);
}
        break;
      case 242: /* transtype ::= */
{
    yymsp[1].minor.yy686 = (int)SYNTAQLITE_TRANSACTION_TYPE_NONE;
}
        break;
      case 243: /* transtype ::= DEFERRED */
{
    yymsp[0].minor.yy686 = (int)SYNTAQLITE_TRANSACTION_TYPE_DEFERRED;
}
        break;
      case 244: /* transtype ::= IMMEDIATE */
{
    yymsp[0].minor.yy686 = (int)SYNTAQLITE_TRANSACTION_TYPE_IMMEDIATE;
}
        break;
      case 245: /* transtype ::= EXCLUSIVE */
{
    yymsp[0].minor.yy686 = (int)SYNTAQLITE_TRANSACTION_TYPE_EXCLUSIVE;
}
        break;
      case 246: /* trans_opt ::= */
{
    yymsp[1].minor.yy482.has_transaction = 0;
    yymsp[1].minor.yy482.name.z = NULL; yymsp[1].minor.yy482.name.n = 0;
}
        break;
      case 247: /* trans_opt ::= TRANSACTION */
{
    yymsp[0].minor.yy482.has_transaction = 1;
    yymsp[0].minor.yy482.name.z = NULL; yymsp[0].minor.yy482.name.n = 0;
}
        break;
      case 248: /* trans_opt ::= TRANSACTION nm */
{
    yymsp[-1].minor.yy482.has_transaction = 1;
    yymsp[-1].minor.yy482.name = yymsp[0].minor.yy0;
}
        break;
      case 251: /* cmd ::= SAVEPOINT nmorerr */
{
    yymsp[-1].minor.yy381 = synq_parse_savepoint_stmt(pCtx,
        SYNTAQLITE_SAVEPOINT_OP_SAVEPOINT,
        yymsp[0].minor.yy381, SYNTAQLITE_BOOL_TRUE, SYNTAQLITE_BOOL_FALSE, SYNQ_NO_SPAN);
}
        break;
      case 252: /* cmd ::= RELEASE savepoint_opt nmorerr */
{
    yymsp[-2].minor.yy381 = synq_parse_savepoint_stmt(pCtx,
        SYNTAQLITE_SAVEPOINT_OP_RELEASE,
        yymsp[0].minor.yy381, yymsp[-1].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_BOOL_FALSE, SYNQ_NO_SPAN);
}
        break;
      case 253: /* cmd ::= ROLLBACK trans_opt TO savepoint_opt nmorerr */
{
    yymsp[-4].minor.yy381 = synq_parse_savepoint_stmt(pCtx,
        SYNTAQLITE_SAVEPOINT_OP_ROLLBACK_TO,
        yymsp[0].minor.yy381, yymsp[-1].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-3].minor.yy482.has_transaction ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-3].minor.yy482.name.z ? synq_span(pCtx, yymsp[-3].minor.yy482.name) : SYNQ_NO_SPAN);
}
        break;
      case 257: /* oneselect ::= SELECT distinct selcollist from where_opt groupby_opt having_opt orderby_opt limit_opt */
{
    yymsp[-8].minor.yy381 = synq_parse_select_stmt(pCtx, (SyntaqliteSelectStmtFlags){.raw = (uint8_t)(yymsp[-7].minor.yy381 & 0xFF)}, yymsp[-6].minor.yy381, yymsp[-5].minor.yy381, yymsp[-4].minor.yy381, yymsp[-3].minor.yy381, yymsp[-2].minor.yy381, yymsp[-1].minor.yy381, yymsp[0].minor.yy381, SYNTAQLITE_NULL_NODE);
}
        break;
      case 258: /* oneselect ::= SELECT distinct selcollist from where_opt groupby_opt having_opt window_clause orderby_opt limit_opt */
{
    yymsp[-9].minor.yy381 = synq_parse_select_stmt(pCtx, (SyntaqliteSelectStmtFlags){.raw = (uint8_t)(yymsp[-8].minor.yy381 & 0xFF)}, yymsp[-7].minor.yy381, yymsp[-6].minor.yy381, yymsp[-5].minor.yy381, yymsp[-4].minor.yy381, yymsp[-3].minor.yy381, yymsp[-1].minor.yy381, yymsp[0].minor.yy381, yymsp[-2].minor.yy381);
}
        break;
      case 259: /* selcollist ::= sclp scanpt expr scanpt as */
{
    uint32_t col = synq_parse_result_column(pCtx, (SyntaqliteResultColumnFlags){0}, yymsp[0].minor.yy119.name, yymsp[0].minor.yy119.has_as, yymsp[-2].minor.yy381);
    yylhsminor.yy381 = synq_parse_result_column_list(pCtx, yymsp[-4].minor.yy381, col);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 260: /* selcollist ::= sclp scanpt STAR */
{
    uint32_t col = synq_parse_result_column(pCtx, (SyntaqliteResultColumnFlags){.raw = 0x01},
                                           SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE,
                                           SYNTAQLITE_NULL_NODE);
    yylhsminor.yy381 = synq_parse_result_column_list(pCtx, yymsp[-2].minor.yy381, col);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 264: /* as ::= AS nmorerr */
{
    yymsp[-1].minor.yy119.name = synq_pass(pCtx, yymsp[0].minor.yy381);
    yymsp[-1].minor.yy119.has_as = 1;
}
        break;
      case 265: /* as ::= ID|STRING */
{
    yylhsminor.yy119.name = synq_parse_ident_name(pCtx, synq_span_dequote(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy119.has_as = 0;
}
  yymsp[0].minor.yy119 = yylhsminor.yy119;
        break;
      case 266: /* as ::= */
{
    yymsp[1].minor.yy119.name = SYNTAQLITE_NULL_NODE;
    yymsp[1].minor.yy119.has_as = 0;
}
        break;
      case 268: /* distinct ::= ALL */
{
    // Bit 2 is STAR in FunctionCallFlags, so ALL takes bit 4 in every set
    // that this value is cast into.
    yymsp[0].minor.yy381 = 4;
}
        break;
      case 275: /* groupby_opt ::= GROUP BY nexprlist */
      case 279: /* orderby_opt ::= ORDER BY sortlist */ yytestcase(yyruleno==279);
{
    yymsp[-2].minor.yy381 = synq_pass(pCtx, yymsp[0].minor.yy381);
}
        break;
      case 281: /* limit_opt ::= LIMIT expr */
{
    yymsp[-1].minor.yy381 = synq_parse_limit_clause(pCtx, yymsp[0].minor.yy381, SYNTAQLITE_NULL_NODE,
                                SYNTAQLITE_BOOL_FALSE);
}
        break;
      case 282: /* limit_opt ::= LIMIT expr OFFSET expr */
{
    yymsp[-3].minor.yy381 = synq_parse_limit_clause(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381, SYNTAQLITE_BOOL_FALSE);
}
        break;
      case 283: /* limit_opt ::= LIMIT expr COMMA expr */
{
    yymsp[-3].minor.yy381 = synq_parse_limit_clause(pCtx, yymsp[0].minor.yy381, yymsp[-2].minor.yy381, SYNTAQLITE_BOOL_TRUE);
}
        break;
      case 284: /* stl_prefix ::= seltablist joinop */
{
    yymsp[-1].minor.yy381 = synq_parse_join_prefix(pCtx, yymsp[-1].minor.yy381, yymsp[0].minor.yy138.join_type, yymsp[0].minor.yy138.modifiers);
}
        break;
      case 286: /* seltablist ::= stl_prefix nm dbnm as on_using */
{
    uint32_t alias = yymsp[-1].minor.yy119.name;
    SyntaqliteBool alias_as = yymsp[-1].minor.yy119.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE;
    SyntaqliteTextSpan table_name;
    SyntaqliteTextSpan schema;
    if (yymsp[-2].minor.yy0.z != NULL) {
        table_name = synq_span_dequote(pCtx, yymsp[-2].minor.yy0);
        schema = synq_span_dequote(pCtx, yymsp[-3].minor.yy0);
    } else {
        table_name = synq_span_dequote(pCtx, yymsp[-3].minor.yy0);
        schema = SYNQ_NO_SPAN;
    }
    uint32_t tref = synq_parse_table_ref(pCtx, table_name, schema,
                                         SYNTAQLITE_BOOL_FALSE,
                                         alias, alias_as, SYNTAQLITE_NULL_NODE,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
    if (yymsp[-4].minor.yy381 == SYNTAQLITE_NULL_NODE) {
        synq_reject_dangling_on_using(pCtx, yymsp[0].minor.yy660);
        yymsp[-4].minor.yy381 = tref;
    } else {
        SyntaqliteNode *pfx = AST_NODE(&pCtx->ast, yymsp[-4].minor.yy381);
        yymsp[-4].minor.yy381 = synq_parse_join_clause(pCtx,
            pfx->join_prefix.join_type,
            pfx->join_prefix.modifiers,
            pfx->join_prefix.source,
            tref, yymsp[0].minor.yy660.on_expr, yymsp[0].minor.yy660.using_cols);
    }
}
        break;
      case 287: /* seltablist ::= stl_prefix nm dbnm as indexed_by on_using */
{
    uint32_t alias = yymsp[-2].minor.yy119.name;
    SyntaqliteBool alias_as = yymsp[-2].minor.yy119.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE;
    SyntaqliteTextSpan table_name;
    SyntaqliteTextSpan schema;
    if (yymsp[-3].minor.yy0.z != NULL) {
        table_name = synq_span_dequote(pCtx, yymsp[-3].minor.yy0);
        schema = synq_span_dequote(pCtx, yymsp[-4].minor.yy0);
    } else {
        table_name = synq_span_dequote(pCtx, yymsp[-4].minor.yy0);
        schema = SYNQ_NO_SPAN;
    }
    SyntaqliteIndexHint ih = (yymsp[-1].minor.yy0.z != NULL) ? SYNTAQLITE_INDEX_HINT_INDEXED
                           : (yymsp[-1].minor.yy0.n == 1)    ? SYNTAQLITE_INDEX_HINT_NOT_INDEXED
                           :                 SYNTAQLITE_INDEX_HINT_DEFAULT;
    uint32_t tref = synq_parse_table_ref(pCtx, table_name, schema,
                                         SYNTAQLITE_BOOL_FALSE,
                                         alias, alias_as, SYNTAQLITE_NULL_NODE,
                                         ih, synq_span(pCtx, yymsp[-1].minor.yy0));
    if (yymsp[-5].minor.yy381 == SYNTAQLITE_NULL_NODE) {
        synq_reject_dangling_on_using(pCtx, yymsp[0].minor.yy660);
        yymsp[-5].minor.yy381 = tref;
    } else {
        SyntaqliteNode *pfx = AST_NODE(&pCtx->ast, yymsp[-5].minor.yy381);
        yymsp[-5].minor.yy381 = synq_parse_join_clause(pCtx,
            pfx->join_prefix.join_type,
            pfx->join_prefix.modifiers,
            pfx->join_prefix.source,
            tref, yymsp[0].minor.yy660.on_expr, yymsp[0].minor.yy660.using_cols);
    }
}
        break;
      case 288: /* seltablist ::= stl_prefix nm dbnm LP exprlist RP as on_using */
{
    uint32_t alias = yymsp[-1].minor.yy119.name;
    SyntaqliteBool alias_as = yymsp[-1].minor.yy119.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE;
    SyntaqliteTextSpan table_name;
    SyntaqliteTextSpan schema;
    if (yymsp[-5].minor.yy0.z != NULL) {
        table_name = synq_span_dequote(pCtx, yymsp[-5].minor.yy0);
        schema = synq_span_dequote(pCtx, yymsp[-6].minor.yy0);
    } else {
        table_name = synq_span_dequote(pCtx, yymsp[-6].minor.yy0);
        schema = SYNQ_NO_SPAN;
    }
    uint32_t tref = synq_parse_table_ref(pCtx, table_name, schema,
                                         SYNTAQLITE_BOOL_TRUE,
                                         alias, alias_as, yymsp[-3].minor.yy381,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
    if (yymsp[-7].minor.yy381 == SYNTAQLITE_NULL_NODE) {
        synq_reject_dangling_on_using(pCtx, yymsp[0].minor.yy660);
        yymsp[-7].minor.yy381 = tref;
    } else {
        SyntaqliteNode *pfx = AST_NODE(&pCtx->ast, yymsp[-7].minor.yy381);
        yymsp[-7].minor.yy381 = synq_parse_join_clause(pCtx,
            pfx->join_prefix.join_type,
            pfx->join_prefix.modifiers,
            pfx->join_prefix.source,
            tref, yymsp[0].minor.yy660.on_expr, yymsp[0].minor.yy660.using_cols);
    }
}
        break;
      case 289: /* seltablist ::= stl_prefix LP select RP as on_using */
{
    pCtx->saw_subquery = 1;
    uint32_t alias = yymsp[-1].minor.yy119.name;
    SyntaqliteBool alias_as = yymsp[-1].minor.yy119.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE;
    uint32_t sub = synq_parse_subquery_table_source(pCtx, yymsp[-3].minor.yy381, alias, alias_as);
    if (yymsp[-5].minor.yy381 == SYNTAQLITE_NULL_NODE) {
        synq_reject_dangling_on_using(pCtx, yymsp[0].minor.yy660);
        yymsp[-5].minor.yy381 = sub;
    } else {
        SyntaqliteNode *pfx = AST_NODE(&pCtx->ast, yymsp[-5].minor.yy381);
        yymsp[-5].minor.yy381 = synq_parse_join_clause(pCtx,
            pfx->join_prefix.join_type,
            pfx->join_prefix.modifiers,
            pfx->join_prefix.source,
            sub, yymsp[0].minor.yy660.on_expr, yymsp[0].minor.yy660.using_cols);
    }
}
        break;
      case 290: /* seltablist ::= stl_prefix LP seltablist RP as on_using */
{
    uint32_t paren = synq_parse_paren_table_source(
        pCtx, yymsp[-3].minor.yy381, yymsp[-1].minor.yy119.name,
        yymsp[-1].minor.yy119.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE);
    if (yymsp[-5].minor.yy381 == SYNTAQLITE_NULL_NODE) {
        synq_reject_dangling_on_using(pCtx, yymsp[0].minor.yy660);
        yymsp[-5].minor.yy381 = paren;
    } else {
        SyntaqliteNode *pfx = AST_NODE(&pCtx->ast, yymsp[-5].minor.yy381);
        yymsp[-5].minor.yy381 = synq_parse_join_clause(pCtx,
            pfx->join_prefix.join_type,
            pfx->join_prefix.modifiers,
            pfx->join_prefix.source,
            paren, yymsp[0].minor.yy660.on_expr, yymsp[0].minor.yy660.using_cols);
    }
}
        break;
      case 291: /* joinop ::= COMMA|JOIN */
{
    yylhsminor.yy138.join_type = (yymsp[0].minor.yy0.type == SYNTAQLITE_TK_COMMA)
        ? SYNTAQLITE_JOIN_TYPE_COMMA
        : SYNTAQLITE_JOIN_TYPE_INNER;
    yylhsminor.yy138.modifiers = SYNTAQLITE_NULL_NODE;
}
  yymsp[0].minor.yy138 = yylhsminor.yy138;
        break;
      case 292: /* joinop ::= JOIN_KW JOIN */
{
    yylhsminor.yy138 = synq_join_operator(pCtx, &yymsp[-1].minor.yy0, NULL, NULL);
}
  yymsp[-1].minor.yy138 = yylhsminor.yy138;
        break;
      case 293: /* joinop ::= JOIN_KW nm JOIN */
{
    yylhsminor.yy138 = synq_join_operator(pCtx, &yymsp[-2].minor.yy0, &yymsp[-1].minor.yy0, NULL);
}
  yymsp[-2].minor.yy138 = yylhsminor.yy138;
        break;
      case 294: /* joinop ::= JOIN_KW nm nm JOIN */
{
    yylhsminor.yy138 = synq_join_operator(pCtx, &yymsp[-3].minor.yy0, &yymsp[-2].minor.yy0, &yymsp[-1].minor.yy0);
}
  yymsp[-3].minor.yy138 = yylhsminor.yy138;
        break;
      case 295: /* on_using ::= ON expr */
{
    yymsp[-1].minor.yy660.on_expr = yymsp[0].minor.yy381;
    yymsp[-1].minor.yy660.using_cols = SYNTAQLITE_NULL_NODE;
}
        break;
      case 296: /* on_using ::= USING LP idlist RP */
{
    yymsp[-3].minor.yy660.on_expr = SYNTAQLITE_NULL_NODE;
    yymsp[-3].minor.yy660.using_cols = yymsp[-1].minor.yy381;
}
        break;
      case 297: /* on_using ::= */
{
    yymsp[1].minor.yy660.on_expr = SYNTAQLITE_NULL_NODE;
    yymsp[1].minor.yy660.using_cols = SYNTAQLITE_NULL_NODE;
}
        break;
      case 298: /* indexed_by ::= INDEXED BY nm */
{
    yymsp[-2].minor.yy0 = yymsp[0].minor.yy0;
}
        break;
      case 299: /* indexed_by ::= NOT INDEXED */
{
    yymsp[-1].minor.yy0.z = NULL; yymsp[-1].minor.yy0.n = 1;
}
        break;
      case 300: /* idlist ::= idlist COMMA nm */
{
    uint32_t col = synq_parse_column_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0), SYNQ_NO_SPAN, SYNQ_NO_SPAN);
    yymsp[-2].minor.yy381 = synq_parse_expr_list(pCtx, yymsp[-2].minor.yy381, col);
}
        break;
      case 301: /* idlist ::= nm */
{
    uint32_t col = synq_parse_column_ref(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0), SYNQ_NO_SPAN, SYNQ_NO_SPAN);
    yylhsminor.yy381 = synq_parse_expr_list(pCtx, SYNTAQLITE_NULL_NODE, col);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 302: /* cmd ::= createkw trigger_decl BEGIN trigger_cmd_list END */
{
    // yymsp[-3].minor.yy381 is a partially-built CreateTriggerStmt, fill in the body
    SyntaqliteNode *trig = AST_NODE(&pCtx->ast, yymsp[-3].minor.yy381);
    trig->create_trigger_stmt.body = yymsp[-1].minor.yy381;
    yymsp[-4].minor.yy381 = synq_pass(pCtx, yymsp[-3].minor.yy381);
}
        break;
      case 303: /* trigger_decl ::= temp TRIGGER ifnotexists nm dbnm trigger_time trigger_event ON fullname foreach_clause when_clause */
{
    SyntaqliteTextSpan trig_name = yymsp[-6].minor.yy0.z ? synq_span(pCtx, yymsp[-6].minor.yy0) : synq_span(pCtx, yymsp[-7].minor.yy0);
    SyntaqliteTextSpan trig_schema = yymsp[-6].minor.yy0.z ? synq_span(pCtx, yymsp[-7].minor.yy0) : SYNQ_NO_SPAN;
    // yylhsminor.yy381 TEMP trigger always lives in the temp schema, so it cannot be qualified.
    if (yymsp[-10].minor.yy234 != SYNTAQLITE_TEMPORARY_QUALIFIER_NONE && yymsp[-6].minor.yy0.z) {
        pCtx->error = 1;
    }
    yylhsminor.yy381 = synq_parse_create_trigger_stmt(pCtx,
        trig_name,
        trig_schema,
        yymsp[-10].minor.yy234,
        (SyntaqliteBool)yymsp[-8].minor.yy686,
        (SyntaqliteTriggerTiming)yymsp[-5].minor.yy686,
        yymsp[-1].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-4].minor.yy381,
        yymsp[-2].minor.yy381,
        yymsp[0].minor.yy381,
        SYNTAQLITE_NULL_NODE);  // body filled in by cmd rule
}
  yymsp[-10].minor.yy381 = yylhsminor.yy381;
        break;
      case 304: /* trigger_time ::= BEFORE|AFTER */
{
    yylhsminor.yy686 = (yymsp[0].minor.yy0.type == SYNTAQLITE_TK_BEFORE) ? (int)SYNTAQLITE_TRIGGER_TIMING_BEFORE
                               : (int)SYNTAQLITE_TRIGGER_TIMING_AFTER;
}
  yymsp[0].minor.yy686 = yylhsminor.yy686;
        break;
      case 305: /* trigger_time ::= INSTEAD OF */
{
    yymsp[-1].minor.yy686 = (int)SYNTAQLITE_TRIGGER_TIMING_INSTEAD_OF;
}
        break;
      case 306: /* trigger_time ::= */
{
    yymsp[1].minor.yy686 = (int)SYNTAQLITE_TRIGGER_TIMING_NONE;
}
        break;
      case 307: /* trigger_event ::= DELETE|INSERT */
{
    SyntaqliteTriggerEventType evt = (yymsp[0].minor.yy0.type == SYNTAQLITE_TK_DELETE)
        ? SYNTAQLITE_TRIGGER_EVENT_TYPE_DELETE
        : SYNTAQLITE_TRIGGER_EVENT_TYPE_INSERT;
    yylhsminor.yy381 = synq_parse_trigger_event(pCtx, evt, SYNTAQLITE_NULL_NODE);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 308: /* trigger_event ::= UPDATE */
{
    yymsp[0].minor.yy381 = synq_parse_trigger_event(pCtx,
        SYNTAQLITE_TRIGGER_EVENT_TYPE_UPDATE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 309: /* trigger_event ::= UPDATE OF idlist */
{
    yymsp[-2].minor.yy381 = synq_parse_trigger_event(pCtx,
        SYNTAQLITE_TRIGGER_EVENT_TYPE_UPDATE, yymsp[0].minor.yy381);
}
        break;
      case 311: /* foreach_clause ::= FOR EACH ROW */
      case 361: /* ifnotexists ::= IF NOT EXISTS */ yytestcase(yyruleno==361);
{
    yymsp[-2].minor.yy686 = 1;
}
        break;
      case 314: /* trigger_cmd_list ::= trigger_cmd_list trigger_cmd SEMI */
{
    yylhsminor.yy381 = synq_parse_trigger_cmd_list(pCtx, yymsp[-2].minor.yy381, yymsp[-1].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 315: /* trigger_cmd_list ::= trigger_cmd SEMI */
{
    yylhsminor.yy381 = synq_parse_trigger_cmd_list(pCtx, SYNTAQLITE_NULL_NODE, yymsp[-1].minor.yy381);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 317: /* trnm ::= nm DOT nm */
{
    yymsp[-2].minor.yy0 = yymsp[0].minor.yy0;
    pCtx->error = 1;
}
        break;
      case 318: /* tridxby ::= */
      case 376: /* vtabarg ::= */ yytestcase(yyruleno==376);
      case 381: /* anylist ::= */ yytestcase(yyruleno==381);
{
    // empty
}
        break;
      case 319: /* tridxby ::= INDEXED BY nm */
      case 320: /* tridxby ::= NOT INDEXED */ yytestcase(yyruleno==320);
{
    pCtx->error = 1;
}
        break;
      case 321: /* trigger_cmd ::= UPDATE orconf trnm tridxby SET setlist from where_opt scanpt */
{
    uint32_t tbl = synq_parse_table_ref(pCtx,
        synq_span(pCtx, yymsp[-6].minor.yy0), SYNQ_NO_SPAN,
        SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_NULL_NODE,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
    yymsp[-8].minor.yy381 = synq_parse_update_stmt(pCtx,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE,
        (SyntaqliteConflictAction)yymsp[-7].minor.yy686, tbl,
        SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN,
        yymsp[-3].minor.yy381, yymsp[-2].minor.yy381, yymsp[-1].minor.yy381, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 322: /* trigger_cmd ::= scanpt insert_cmd INTO trnm idlist_opt select upsert scanpt */
{
    uint32_t tbl = synq_parse_table_ref(pCtx,
        synq_span(pCtx, yymsp[-4].minor.yy0), SYNQ_NO_SPAN,
        SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_NULL_NODE,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
    if (yymsp[-1].minor.yy54.returning != SYNTAQLITE_NULL_NODE) {
        pCtx->error = 1;
    }
    yymsp[-7].minor.yy381 = synq_parse_insert_stmt(pCtx,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE,
        yymsp[-6].minor.yy90.keyword, yymsp[-6].minor.yy90.conflict_action, tbl, yymsp[-3].minor.yy381, yymsp[-2].minor.yy381,
        yymsp[-1].minor.yy54.clauses, yymsp[-1].minor.yy54.returning);
}
        break;
      case 323: /* trigger_cmd ::= DELETE FROM trnm tridxby where_opt scanpt */
{
    uint32_t tbl = synq_parse_table_ref(pCtx,
        synq_span(pCtx, yymsp[-3].minor.yy0), SYNQ_NO_SPAN,
        SYNTAQLITE_BOOL_FALSE,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE, SYNTAQLITE_NULL_NODE,
                                         SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN);
    yymsp[-5].minor.yy381 = synq_parse_delete_stmt(pCtx,
        SYNTAQLITE_NULL_NODE, SYNTAQLITE_BOOL_FALSE,
        tbl,
        SYNTAQLITE_INDEX_HINT_DEFAULT, SYNQ_NO_SPAN,
        yymsp[-1].minor.yy381, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE, SYNTAQLITE_NULL_NODE);
}
        break;
      case 325: /* cmd ::= PRAGMA nm dbnm */
{
    SyntaqliteTextSpan name_span = yymsp[0].minor.yy0.z ? synq_span(pCtx, yymsp[0].minor.yy0) : synq_span(pCtx, yymsp[-1].minor.yy0);
    SyntaqliteTextSpan schema_span = yymsp[0].minor.yy0.z ? synq_span(pCtx, yymsp[-1].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-2].minor.yy381 = synq_parse_pragma_stmt(pCtx, name_span, schema_span, SYNQ_NO_SPAN, SYNTAQLITE_PRAGMA_FORM_BARE);
}
        break;
      case 326: /* cmd ::= PRAGMA nm dbnm EQ nmnum */
      case 328: /* cmd ::= PRAGMA nm dbnm EQ minus_num */ yytestcase(yyruleno==328);
{
    SyntaqliteTextSpan name_span = yymsp[-2].minor.yy0.z ? synq_span(pCtx, yymsp[-2].minor.yy0) : synq_span(pCtx, yymsp[-3].minor.yy0);
    SyntaqliteTextSpan schema_span = yymsp[-2].minor.yy0.z ? synq_span(pCtx, yymsp[-3].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-4].minor.yy381 = synq_parse_pragma_stmt(pCtx, name_span, schema_span, synq_span(pCtx, yymsp[0].minor.yy0), SYNTAQLITE_PRAGMA_FORM_EQ);
}
        break;
      case 327: /* cmd ::= PRAGMA nm dbnm LP nmnum RP */
      case 329: /* cmd ::= PRAGMA nm dbnm LP minus_num RP */ yytestcase(yyruleno==329);
{
    SyntaqliteTextSpan name_span = yymsp[-3].minor.yy0.z ? synq_span(pCtx, yymsp[-3].minor.yy0) : synq_span(pCtx, yymsp[-4].minor.yy0);
    SyntaqliteTextSpan schema_span = yymsp[-3].minor.yy0.z ? synq_span(pCtx, yymsp[-4].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-5].minor.yy381 = synq_parse_pragma_stmt(pCtx, name_span, schema_span, synq_span(pCtx, yymsp[-1].minor.yy0), SYNTAQLITE_PRAGMA_FORM_CALL);
}
        break;
      case 335: /* plus_num ::= PLUS INTEGER|FLOAT */
{
    yymsp[-1].minor.yy0 = yymsp[0].minor.yy0;
}
        break;
      case 337: /* minus_num ::= MINUS INTEGER|FLOAT */
{
    // Build a token that spans from the MINUS sign through the number
    yylhsminor.yy0.z = yymsp[-1].minor.yy0.z;
    yylhsminor.yy0.n = (int)(yymsp[0].minor.yy0.z - yymsp[-1].minor.yy0.z) + yymsp[0].minor.yy0.n;
    yylhsminor.yy0.offset = yymsp[-1].minor.yy0.offset;
    yylhsminor.yy0.layer_id = yymsp[-1].minor.yy0.layer_id;
}
  yymsp[-1].minor.yy0 = yylhsminor.yy0;
        break;
      case 340: /* cmd ::= ANALYZE */
{
    yymsp[0].minor.yy381 = synq_parse_analyze_or_reindex_stmt(pCtx,
        SYNQ_NO_SPAN,
        SYNQ_NO_SPAN,
        SYNTAQLITE_ANALYZE_OR_REINDEX_OP_ANALYZE);
}
        break;
      case 341: /* cmd ::= ANALYZE nm dbnm */
{
    SyntaqliteTextSpan name_span = yymsp[0].minor.yy0.z ? synq_span(pCtx, yymsp[0].minor.yy0) : synq_span(pCtx, yymsp[-1].minor.yy0);
    SyntaqliteTextSpan schema_span = yymsp[0].minor.yy0.z ? synq_span(pCtx, yymsp[-1].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-2].minor.yy381 = synq_parse_analyze_or_reindex_stmt(pCtx, name_span, schema_span, SYNTAQLITE_ANALYZE_OR_REINDEX_OP_ANALYZE);
}
        break;
      case 342: /* cmd ::= REINDEX */
{
    yymsp[0].minor.yy381 = synq_parse_analyze_or_reindex_stmt(pCtx,
        SYNQ_NO_SPAN,
        SYNQ_NO_SPAN,
        SYNTAQLITE_ANALYZE_OR_REINDEX_OP_REINDEX);
}
        break;
      case 343: /* cmd ::= REINDEX nm dbnm */
{
    SyntaqliteTextSpan name_span = yymsp[0].minor.yy0.z ? synq_span(pCtx, yymsp[0].minor.yy0) : synq_span(pCtx, yymsp[-1].minor.yy0);
    SyntaqliteTextSpan schema_span = yymsp[0].minor.yy0.z ? synq_span(pCtx, yymsp[-1].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-2].minor.yy381 = synq_parse_analyze_or_reindex_stmt(pCtx, name_span, schema_span, 1);
}
        break;
      case 344: /* cmd ::= ATTACH database_kw_opt expr AS expr key_opt */
{
    yymsp[-5].minor.yy381 = synq_parse_attach_stmt(pCtx,
        yymsp[-4].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE, yymsp[-3].minor.yy381, yymsp[-1].minor.yy381, yymsp[0].minor.yy381);
}
        break;
      case 345: /* cmd ::= DETACH database_kw_opt expr */
{
    yymsp[-2].minor.yy381 = synq_parse_detach_stmt(pCtx,
        yymsp[-1].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE, yymsp[0].minor.yy381);
}
        break;
      case 350: /* cmd ::= VACUUM vinto */
{
    yymsp[-1].minor.yy381 = synq_parse_vacuum_stmt(pCtx,
        SYNQ_NO_SPAN,
        yymsp[0].minor.yy381);
}
        break;
      case 351: /* cmd ::= VACUUM nm vinto */
{
    yymsp[-2].minor.yy381 = synq_parse_vacuum_stmt(pCtx,
        synq_span(pCtx, yymsp[-1].minor.yy0),
        yymsp[0].minor.yy381);
}
        break;
      case 354: /* ecmd ::= explain cmdx SEMI */
{
    (void)yymsp[-2].minor.yy686;
    yylhsminor.yy381 = synq_pass(pCtx, yymsp[-1].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 355: /* explain ::= EXPLAIN */
{
    yymsp[0].minor.yy686 = 1;
    pCtx->pending_explain_mode = 1;
}
        break;
      case 356: /* explain ::= EXPLAIN QUERY PLAN */
{
    yymsp[-2].minor.yy686 = 2;
    pCtx->pending_explain_mode = 2;
}
        break;
      case 357: /* cmd ::= createkw uniqueflag INDEX ifnotexists nm dbnm ON nm LP sortlist RP where_opt */
{
    SyntaqliteTextSpan idx_name = yymsp[-6].minor.yy0.z ? synq_span(pCtx, yymsp[-6].minor.yy0) : synq_span(pCtx, yymsp[-7].minor.yy0);
    SyntaqliteTextSpan idx_schema = yymsp[-6].minor.yy0.z ? synq_span(pCtx, yymsp[-7].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-11].minor.yy381 = synq_parse_create_index_stmt(pCtx,
        idx_name,
        idx_schema,
        synq_span(pCtx, yymsp[-4].minor.yy0),
        (SyntaqliteBool)yymsp[-10].minor.yy686,
        (SyntaqliteBool)yymsp[-8].minor.yy686,
        yymsp[-2].minor.yy381,
        yymsp[0].minor.yy381);
}
        break;
      case 362: /* cmd ::= createkw temp VIEW ifnotexists nm dbnm eidlist_opt AS select */
{
    SyntaqliteTextSpan view_name = yymsp[-3].minor.yy0.z ? synq_span(pCtx, yymsp[-3].minor.yy0) : synq_span(pCtx, yymsp[-4].minor.yy0);
    SyntaqliteTextSpan view_schema = yymsp[-3].minor.yy0.z ? synq_span(pCtx, yymsp[-4].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-8].minor.yy381 = synq_parse_create_view_stmt(pCtx,
        view_name,
        view_schema,
        yymsp[-7].minor.yy234,
        (SyntaqliteBool)yymsp[-5].minor.yy686,
        yymsp[-2].minor.yy381,
        yymsp[0].minor.yy381);
}
        break;
      case 364: /* temp ::= TEMP */
{
    // SQLite maps both spellings to TEMP; retain the authored choice here.
    yylhsminor.yy234 = yymsp[0].minor.yy0.n == 4 ? SYNTAQLITE_TEMPORARY_QUALIFIER_TEMP
                 : SYNTAQLITE_TEMPORARY_QUALIFIER_TEMPORARY;
}
  yymsp[0].minor.yy234 = yylhsminor.yy234;
        break;
      case 365: /* temp ::= */
{
    yymsp[1].minor.yy234 = SYNTAQLITE_TEMPORARY_QUALIFIER_NONE;
}
        break;
      case 366: /* values ::= VALUES LP nexprlist RP */
{
    yymsp[-3].minor.yy381 = synq_parse_values_row_list(pCtx, SYNTAQLITE_NULL_NODE, yymsp[-1].minor.yy381);
}
        break;
      case 367: /* mvalues ::= values COMMA LP nexprlist RP */
      case 368: /* mvalues ::= mvalues COMMA LP nexprlist RP */ yytestcase(yyruleno==368);
{
    yymsp[-4].minor.yy381 = synq_parse_values_row_list(pCtx, yymsp[-4].minor.yy381, yymsp[-1].minor.yy381);
}
        break;
      case 369: /* oneselect ::= values */
      case 370: /* oneselect ::= mvalues */ yytestcase(yyruleno==370);
{
    yylhsminor.yy381 = synq_parse_values_clause(pCtx, yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 372: /* cmd ::= create_vtab LP vtabarglist RP */
{
    // Capture module arguments span (content between parens).
    // Use token offsets/layer_id so this works correctly when the statement
    // is produced by a macro expansion; LP and RP share a layer within the
    // same reduction.
    SyntaqliteNode *vtab = AST_NODE(&pCtx->ast, yymsp[-3].minor.yy381);
    uint32_t args_start = yymsp[-2].minor.yy0.offset + yymsp[-2].minor.yy0.n;
    uint32_t args_end = yymsp[0].minor.yy0.offset;
    vtab->create_virtual_table_stmt.has_module_args = SYNTAQLITE_BOOL_TRUE;
    vtab->create_virtual_table_stmt.module_args = (SyntaqliteTextSpan){
        .offset = args_start,
        .length = args_end - args_start,
        .flags = 0,
        ._layer_id = yymsp[-2].minor.yy0.layer_id,
    };
    yylhsminor.yy381 = synq_pass(pCtx, yymsp[-3].minor.yy381);
}
  yymsp[-3].minor.yy381 = yylhsminor.yy381;
        break;
      case 373: /* create_vtab ::= createkw VIRTUAL TABLE ifnotexists nm dbnm USING nm */
{
    SyntaqliteTextSpan tbl_name = yymsp[-2].minor.yy0.z ? synq_span(pCtx, yymsp[-2].minor.yy0) : synq_span(pCtx, yymsp[-3].minor.yy0);
    SyntaqliteTextSpan tbl_schema = yymsp[-2].minor.yy0.z ? synq_span(pCtx, yymsp[-3].minor.yy0) : SYNQ_NO_SPAN;
    yymsp[-7].minor.yy381 = synq_parse_create_virtual_table_stmt(pCtx,
        tbl_name,
        tbl_schema,
        synq_span(pCtx, yymsp[0].minor.yy0),
        (SyntaqliteBool)yymsp[-4].minor.yy686,
        SYNTAQLITE_BOOL_FALSE,
        SYNQ_NO_SPAN);  // module_args = none by default
}
        break;
      case 374: /* vtabarglist ::= vtabarg */
      case 375: /* vtabarglist ::= vtabarglist COMMA vtabarg */ yytestcase(yyruleno==375);
      case 377: /* vtabarg ::= vtabarg vtabargtoken */ yytestcase(yyruleno==377);
      case 378: /* vtabargtoken ::= ANY */ yytestcase(yyruleno==378);
      case 379: /* vtabargtoken ::= lp anylist RP */ yytestcase(yyruleno==379);
      case 380: /* lp ::= LP */ yytestcase(yyruleno==380);
      case 382: /* anylist ::= anylist LP anylist RP */ yytestcase(yyruleno==382);
      case 383: /* anylist ::= anylist ANY */ yytestcase(yyruleno==383);
{
    // consumed
}
        break;
      case 384: /* windowdefn_list ::= windowdefn */
{
    yylhsminor.yy381 = synq_parse_named_window_def_list(pCtx, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 385: /* windowdefn_list ::= windowdefn_list COMMA windowdefn */
{
    yylhsminor.yy381 = synq_parse_named_window_def_list(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 386: /* windowdefn ::= nm AS LP window RP */
{
    yylhsminor.yy381 = synq_parse_named_window_def(pCtx,
        synq_span(pCtx, yymsp[-4].minor.yy0),
        yymsp[-1].minor.yy381);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 387: /* window ::= PARTITION BY nexprlist orderby_opt frame_opt */
{
    yymsp[-4].minor.yy381 = synq_parse_window_def(pCtx,
        SYNQ_NO_SPAN,
        SYNQ_NO_SPAN,
        yymsp[-2].minor.yy381,
        yymsp[-1].minor.yy381,
        yymsp[0].minor.yy381);
}
        break;
      case 388: /* window ::= nm PARTITION BY nexprlist orderby_opt frame_opt */
{
    yylhsminor.yy381 = synq_parse_window_def(pCtx,
        SYNQ_NO_SPAN,
        synq_span(pCtx, yymsp[-5].minor.yy0),
        yymsp[-2].minor.yy381,
        yymsp[-1].minor.yy381,
        yymsp[0].minor.yy381);
}
  yymsp[-5].minor.yy381 = yylhsminor.yy381;
        break;
      case 389: /* window ::= ORDER BY sortlist frame_opt */
{
    yymsp[-3].minor.yy381 = synq_parse_window_def(pCtx,
        SYNQ_NO_SPAN,
        SYNQ_NO_SPAN,
        SYNTAQLITE_NULL_NODE,
        yymsp[-1].minor.yy381,
        yymsp[0].minor.yy381);
}
        break;
      case 390: /* window ::= nm ORDER BY sortlist frame_opt */
{
    yylhsminor.yy381 = synq_parse_window_def(pCtx,
        SYNQ_NO_SPAN,
        synq_span(pCtx, yymsp[-4].minor.yy0),
        SYNTAQLITE_NULL_NODE,
        yymsp[-1].minor.yy381,
        yymsp[0].minor.yy381);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 391: /* window ::= frame_opt */
{
    yylhsminor.yy381 = synq_parse_window_def(pCtx,
        SYNQ_NO_SPAN,
        SYNQ_NO_SPAN,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE,
        yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 392: /* window ::= nm frame_opt */
{
    yylhsminor.yy381 = synq_parse_window_def(pCtx,
        SYNQ_NO_SPAN,
        synq_span(pCtx, yymsp[-1].minor.yy0),
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE,
        yymsp[0].minor.yy381);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 394: /* frame_opt ::= range_or_rows frame_bound_s frame_exclude_opt */
{
    // Preserve shorthand: the semantic end is CURRENT ROW, but no end-bound
    // syntax was authored, so do not manufacture a node for it.
    yylhsminor.yy381 = synq_parse_frame_spec(pCtx,
        (SyntaqliteFrameType)yymsp[-2].minor.yy686,
        (SyntaqliteFrameExclude)yymsp[0].minor.yy686,
        yymsp[-1].minor.yy381,
        SYNTAQLITE_NULL_NODE);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 395: /* frame_opt ::= range_or_rows BETWEEN frame_bound_s AND frame_bound_e frame_exclude_opt */
{
    yylhsminor.yy381 = synq_parse_frame_spec(pCtx,
        (SyntaqliteFrameType)yymsp[-5].minor.yy686,
        (SyntaqliteFrameExclude)yymsp[0].minor.yy686,
        yymsp[-3].minor.yy381,
        yymsp[-1].minor.yy381);
}
  yymsp[-5].minor.yy381 = yylhsminor.yy381;
        break;
      case 396: /* range_or_rows ::= RANGE|ROWS|GROUPS */
{
    switch (yymsp[0].minor.yy0.type) {
        case SYNTAQLITE_TK_RANGE:  yylhsminor.yy686 = SYNTAQLITE_FRAME_TYPE_RANGE; break;
        case SYNTAQLITE_TK_ROWS:   yylhsminor.yy686 = SYNTAQLITE_FRAME_TYPE_ROWS; break;
        default:        yylhsminor.yy686 = SYNTAQLITE_FRAME_TYPE_GROUPS; break;
    }
}
  yymsp[0].minor.yy686 = yylhsminor.yy686;
        break;
      case 398: /* frame_bound_s ::= UNBOUNDED PRECEDING */
{
    yymsp[-1].minor.yy381 = synq_parse_frame_bound(pCtx,
        SYNTAQLITE_FRAME_BOUND_TYPE_UNBOUNDED_PRECEDING,
        SYNTAQLITE_NULL_NODE);
}
        break;
      case 400: /* frame_bound_e ::= UNBOUNDED FOLLOWING */
{
    yymsp[-1].minor.yy381 = synq_parse_frame_bound(pCtx,
        SYNTAQLITE_FRAME_BOUND_TYPE_UNBOUNDED_FOLLOWING,
        SYNTAQLITE_NULL_NODE);
}
        break;
      case 401: /* frame_bound ::= expr PRECEDING|FOLLOWING */
{
    SyntaqliteFrameBoundType bt = (yymsp[0].minor.yy0.type == SYNTAQLITE_TK_PRECEDING)
        ? SYNTAQLITE_FRAME_BOUND_TYPE_EXPR_PRECEDING
        : SYNTAQLITE_FRAME_BOUND_TYPE_EXPR_FOLLOWING;
    yylhsminor.yy381 = synq_parse_frame_bound(pCtx, bt, yymsp[-1].minor.yy381);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 402: /* frame_bound ::= CURRENT ROW */
{
    yymsp[-1].minor.yy381 = synq_parse_frame_bound(pCtx,
        SYNTAQLITE_FRAME_BOUND_TYPE_CURRENT_ROW,
        SYNTAQLITE_NULL_NODE);
}
        break;
      case 403: /* frame_exclude_opt ::= */
{
    yymsp[1].minor.yy686 = SYNTAQLITE_FRAME_EXCLUDE_NONE;
}
        break;
      case 405: /* frame_exclude ::= NO OTHERS */
{
    yymsp[-1].minor.yy686 = SYNTAQLITE_FRAME_EXCLUDE_NO_OTHERS;
}
        break;
      case 406: /* frame_exclude ::= CURRENT ROW */
{
    yymsp[-1].minor.yy686 = SYNTAQLITE_FRAME_EXCLUDE_CURRENT_ROW;
}
        break;
      case 407: /* frame_exclude ::= GROUP|TIES */
{
    yylhsminor.yy686 = (yymsp[0].minor.yy0.type == SYNTAQLITE_TK_GROUP)
        ? SYNTAQLITE_FRAME_EXCLUDE_GROUP
        : SYNTAQLITE_FRAME_EXCLUDE_TIES;
}
  yymsp[0].minor.yy686 = yylhsminor.yy686;
        break;
      case 409: /* filter_over ::= filter_clause over_clause */
{
    // Unpack the over_clause FilterOver to combine with filter expr
    SyntaqliteFilterOver *fo_over = AST_NODE_AS(SyntaqliteFilterOver, &pCtx->ast, yymsp[0].minor.yy381);
    yylhsminor.yy381 = synq_parse_filter_over(pCtx,
        yymsp[-1].minor.yy381,
        fo_over->over_def,
        SYNQ_NO_SPAN);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 411: /* filter_over ::= filter_clause */
{
    yylhsminor.yy381 = synq_parse_filter_over(pCtx,
        yymsp[0].minor.yy381,
        SYNTAQLITE_NULL_NODE,
        SYNQ_NO_SPAN);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 412: /* over_clause ::= OVER LP window RP */
{
    yymsp[-3].minor.yy381 = synq_parse_filter_over(pCtx,
        SYNTAQLITE_NULL_NODE,
        yymsp[-1].minor.yy381,
        SYNQ_NO_SPAN);
}
        break;
      case 413: /* over_clause ::= OVER nm */
{
    uint32_t wdef = synq_parse_window_def(pCtx,
        synq_span(pCtx, yymsp[0].minor.yy0),
        SYNQ_NO_SPAN,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE,
        SYNTAQLITE_NULL_NODE);
    yymsp[-1].minor.yy381 = synq_parse_filter_over(pCtx,
        SYNTAQLITE_NULL_NODE,
        wdef,
        SYNQ_NO_SPAN);
}
        break;
      case 414: /* filter_clause ::= FILTER LP WHERE expr RP */
{
    yymsp[-4].minor.yy381 = synq_pass(pCtx, yymsp[-1].minor.yy381);
}
        break;
      case 418: /* perfetto_arg_type ::= ID LP ID DOT ID RP */
{
    synq_mark_as_type(pCtx, yymsp[-5].minor.yy0);
    yylhsminor.yy0 = (SynqParseToken){
        .z = yymsp[-5].minor.yy0.z,
        .n = (uint32_t)(yymsp[0].minor.yy0.z + yymsp[0].minor.yy0.n - yymsp[-5].minor.yy0.z),
        .type = yymsp[-5].minor.yy0.type,
        .token_idx = yymsp[-5].minor.yy0.token_idx,
        .offset = yymsp[-5].minor.yy0.offset,
        .layer_id = yymsp[-5].minor.yy0.layer_id,
    };
}
  yymsp[-5].minor.yy0 = yylhsminor.yy0;
        break;
      case 419: /* perfetto_arg_def_list ::= */
      case 425: /* perfetto_table_schema ::= */ yytestcase(yyruleno==425);
      case 427: /* perfetto_table_impl ::= */ yytestcase(yyruleno==427);
      case 433: /* perfetto_macro_arg_list ::= */ yytestcase(yyruleno==433);
      case 457: /* perfetto_pipe_stage_list ::= */ yytestcase(yyruleno==457);
      case 463: /* perfetto_per ::= */ yytestcase(yyruleno==463);
{ yymsp[1].minor.yy381 = SYNTAQLITE_NULL_NODE; }
        break;
      case 420: /* perfetto_arg_def_list ::= perfetto_arg_def_list_ne */
      case 434: /* perfetto_macro_arg_list ::= perfetto_macro_arg_list_ne */ yytestcase(yyruleno==434);
      case 467: /* cmd ::= perfetto_pipeline */ yytestcase(yyruleno==467);
{ yylhsminor.yy381 = yymsp[0].minor.yy381; }
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 421: /* perfetto_arg_def_list_ne ::= ID perfetto_arg_type */
{
    uint32_t arg = synq_parse_perfetto_arg_def(pCtx,
        synq_parse_ident_name(pCtx, synq_span(pCtx, yymsp[-1].minor.yy0)), synq_span(pCtx, yymsp[0].minor.yy0),
        SYNTAQLITE_BOOL_FALSE);
    yylhsminor.yy381 = synq_parse_perfetto_arg_def_list(pCtx, SYNTAQLITE_NULL_NODE, arg);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 422: /* perfetto_arg_def_list_ne ::= perfetto_arg_def_list_ne COMMA ID perfetto_arg_type */
{
    uint32_t arg = synq_parse_perfetto_arg_def(pCtx,
        synq_parse_ident_name(pCtx, synq_span(pCtx, yymsp[-1].minor.yy0)), synq_span(pCtx, yymsp[0].minor.yy0),
        SYNTAQLITE_BOOL_FALSE);
    yylhsminor.yy381 = synq_parse_perfetto_arg_def_list(pCtx, yymsp[-3].minor.yy381, arg);
}
  yymsp[-3].minor.yy381 = yylhsminor.yy381;
        break;
      case 423: /* perfetto_arg_def_list_ne ::= ID perfetto_arg_type DOT DOT DOT */
{
    uint32_t arg = synq_parse_perfetto_arg_def(pCtx,
        synq_parse_ident_name(pCtx, synq_span(pCtx, yymsp[-4].minor.yy0)), synq_span(pCtx, yymsp[-3].minor.yy0),
        SYNTAQLITE_BOOL_TRUE);
    yylhsminor.yy381 = synq_parse_perfetto_arg_def_list(pCtx, SYNTAQLITE_NULL_NODE, arg);
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 424: /* perfetto_arg_def_list_ne ::= perfetto_arg_def_list_ne COMMA ID perfetto_arg_type DOT DOT DOT */
{
    uint32_t arg = synq_parse_perfetto_arg_def(pCtx,
        synq_parse_ident_name(pCtx, synq_span(pCtx, yymsp[-4].minor.yy0)), synq_span(pCtx, yymsp[-3].minor.yy0),
        SYNTAQLITE_BOOL_TRUE);
    yylhsminor.yy381 = synq_parse_perfetto_arg_def_list(pCtx, yymsp[-6].minor.yy381, arg);
}
  yymsp[-6].minor.yy381 = yylhsminor.yy381;
        break;
      case 426: /* perfetto_table_schema ::= LP perfetto_arg_def_list_ne RP */
{ yymsp[-2].minor.yy381 = yymsp[-1].minor.yy381; }
        break;
      case 428: /* perfetto_table_impl ::= USING ID */
{
    yymsp[-1].minor.yy381 = synq_parse_perfetto_table_impl(pCtx, synq_span(pCtx, yymsp[0].minor.yy0));
}
        break;
      case 429: /* perfetto_return_type ::= ID */
{
    synq_mark_as_type(pCtx, yymsp[0].minor.yy0);
    yylhsminor.yy381 = synq_parse_perfetto_return_type(pCtx,
        SYNTAQLITE_PERFETTO_RETURN_KIND_SCALAR,
        synq_span(pCtx, yymsp[0].minor.yy0),
        SYNTAQLITE_NULL_NODE);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 430: /* perfetto_return_type ::= TABLE LP perfetto_arg_def_list_ne RP */
{
    yymsp[-3].minor.yy381 = synq_parse_perfetto_return_type(pCtx,
        SYNTAQLITE_PERFETTO_RETURN_KIND_TABLE,
        SYNQ_NO_SPAN,
        yymsp[-1].minor.yy381);
}
        break;
      case 431: /* perfetto_indexed_col_list ::= ID */
{
    uint32_t col = synq_parse_perfetto_indexed_column(pCtx, synq_span(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy381 = synq_parse_perfetto_indexed_column_list(pCtx, SYNTAQLITE_NULL_NODE, col);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 432: /* perfetto_indexed_col_list ::= perfetto_indexed_col_list COMMA ID */
{
    uint32_t col = synq_parse_perfetto_indexed_column(pCtx, synq_span(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy381 = synq_parse_perfetto_indexed_column_list(pCtx, yymsp[-2].minor.yy381, col);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 435: /* perfetto_macro_arg_list_ne ::= ID ID */
{
    synq_mark_as_type(pCtx, yymsp[0].minor.yy0);
    uint32_t arg = synq_parse_perfetto_macro_arg(pCtx,
        synq_span(pCtx, yymsp[-1].minor.yy0), synq_span(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy381 = synq_parse_perfetto_macro_arg_list(pCtx, SYNTAQLITE_NULL_NODE, arg);
}
  yymsp[-1].minor.yy381 = yylhsminor.yy381;
        break;
      case 436: /* perfetto_macro_arg_list_ne ::= perfetto_macro_arg_list_ne COMMA ID ID */
{
    synq_mark_as_type(pCtx, yymsp[0].minor.yy0);
    uint32_t arg = synq_parse_perfetto_macro_arg(pCtx,
        synq_span(pCtx, yymsp[-1].minor.yy0), synq_span(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy381 = synq_parse_perfetto_macro_arg_list(pCtx, yymsp[-3].minor.yy381, arg);
}
  yymsp[-3].minor.yy381 = yylhsminor.yy381;
        break;
      case 437: /* perfetto_module_name ::= ID|STAR|INTERSECT */
      case 476: /* perfetto_macro_body ::= ANY */ yytestcase(yyruleno==476);
{ yylhsminor.yy0 = yymsp[0].minor.yy0; }
  yymsp[0].minor.yy0 = yylhsminor.yy0;
        break;
      case 438: /* perfetto_module_name ::= perfetto_module_name DOT ID|STAR|INTERSECT */
{
    yylhsminor.yy0 = (SynqParseToken){
        .z = yymsp[-2].minor.yy0.z,
        .n = (uint32_t)(yymsp[0].minor.yy0.z + yymsp[0].minor.yy0.n - yymsp[-2].minor.yy0.z),
        .type = yymsp[-2].minor.yy0.type,
        .token_idx = yymsp[-2].minor.yy0.token_idx,
        .offset = yymsp[-2].minor.yy0.offset,
        .layer_id = yymsp[-2].minor.yy0.layer_id,
    };
}
  yymsp[-2].minor.yy0 = yylhsminor.yy0;
        break;
      case 439: /* select_body_start ::= */
{ yymsp[1].minor.yy381 = pCtx->cur_shift_start; }
        break;
      case 440: /* select_body_end ::= */
{ yymsp[1].minor.yy381 = pCtx->last_shifted_end; }
        break;
      case 441: /* perfetto_pipe ::= BITOR GT */
{
    if (yymsp[-1].minor.yy0.layer_id != yymsp[0].minor.yy0.layer_id || yymsp[-1].minor.yy0.offset + yymsp[-1].minor.yy0.n != yymsp[0].minor.yy0.offset) {
        pCtx->error = 1;
    }
    yylhsminor.yy686 = 0;
}
  yymsp[-1].minor.yy686 = yylhsminor.yy686;
        break;
      case 442: /* perfetto_pipe_source ::= nm dbnm as */
{
    SyntaqliteTextSpan table_name;
    SyntaqliteTextSpan schema;
    if (yymsp[-1].minor.yy0.z != NULL) {
        table_name = synq_span_dequote(pCtx, yymsp[-1].minor.yy0);
        schema = synq_span_dequote(pCtx, yymsp[-2].minor.yy0);
    } else {
        table_name = synq_span_dequote(pCtx, yymsp[-2].minor.yy0);
        schema = SYNQ_NO_SPAN;
    }
    yylhsminor.yy381 = synq_parse_perfetto_pipe_source(pCtx, table_name, schema,
        SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy119.name,
        yymsp[0].minor.yy119.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 443: /* perfetto_pipe_source ::= LP select RP as */
{
    yymsp[-3].minor.yy381 = synq_parse_perfetto_pipe_source(pCtx, SYNQ_NO_SPAN, SYNQ_NO_SPAN,
        yymsp[-2].minor.yy381, yymsp[0].minor.yy119.name, yymsp[0].minor.yy119.has_as ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE);
}
        break;
      case 444: /* perfetto_tree_direction ::= UP */
{ yymsp[0].minor.yy686 = SYNTAQLITE_PERFETTO_TREE_DIRECTION_UP; }
        break;
      case 445: /* perfetto_tree_direction ::= DOWN */
{ yymsp[0].minor.yy686 = SYNTAQLITE_PERFETTO_TREE_DIRECTION_DOWN; }
        break;
      case 446: /* perfetto_tree_aggregate ::= expr AS nm */
{
    yylhsminor.yy381 = synq_parse_perfetto_tree_aggregate(pCtx, yymsp[-2].minor.yy381,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0));
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 447: /* perfetto_tree_aggregate_list ::= perfetto_tree_aggregate */
{
    yylhsminor.yy381 = synq_parse_perfetto_tree_aggregate_list(pCtx, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 448: /* perfetto_tree_aggregate_list ::= perfetto_tree_aggregate_list COMMA perfetto_tree_aggregate */
{
    yylhsminor.yy381 = synq_parse_perfetto_tree_aggregate_list(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 449: /* perfetto_pipe_select_item ::= nm */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_column(pCtx, SYNQ_NO_SPAN,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0), SYNQ_NO_SPAN);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 450: /* perfetto_pipe_select_item ::= nm AS nm */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_column(pCtx, SYNQ_NO_SPAN,
        synq_span_dequote(pCtx, yymsp[-2].minor.yy0), synq_span_dequote(pCtx, yymsp[0].minor.yy0));
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 451: /* perfetto_pipe_select_item ::= nm DOT nm */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_column(pCtx, synq_span_dequote(pCtx, yymsp[-2].minor.yy0),
        synq_span_dequote(pCtx, yymsp[0].minor.yy0), SYNQ_NO_SPAN);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 452: /* perfetto_pipe_select_item ::= nm DOT nm AS nm */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_column(pCtx, synq_span_dequote(pCtx, yymsp[-4].minor.yy0),
        synq_span_dequote(pCtx, yymsp[-2].minor.yy0), synq_span_dequote(pCtx, yymsp[0].minor.yy0));
}
  yymsp[-4].minor.yy381 = yylhsminor.yy381;
        break;
      case 453: /* perfetto_pipe_select_list ::= perfetto_pipe_select_item */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_column_list(pCtx, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 454: /* perfetto_pipe_select_list ::= perfetto_pipe_select_list COMMA perfetto_pipe_select_item */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_column_list(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 455: /* perfetto_pipe_stage ::= SELECT perfetto_pipe_select_list */
{
    yymsp[-1].minor.yy381 = synq_parse_perfetto_pipe_select(pCtx, yymsp[0].minor.yy381);
}
        break;
      case 456: /* perfetto_pipe_stage ::= TREE ACCUMULATE perfetto_tree_direction perfetto_tree_aggregate_list */
{
    yymsp[-3].minor.yy381 = synq_parse_perfetto_tree_accumulate(pCtx,
        (SyntaqlitePerfettoTreeDirection)yymsp[-1].minor.yy686, yymsp[0].minor.yy381);
}
        break;
      case 458: /* perfetto_pipe_stage_list ::= perfetto_pipe_stage_list perfetto_pipe perfetto_pipe_stage */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_stage_list(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 459: /* perfetto_pipe_source_list ::= perfetto_pipe_source */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_source_list(pCtx, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 460: /* perfetto_pipe_source_list ::= perfetto_pipe_source_list COMMA perfetto_pipe_source */
{
    yylhsminor.yy381 = synq_parse_perfetto_pipe_source_list(pCtx, yymsp[-2].minor.yy381, yymsp[0].minor.yy381);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 461: /* perfetto_per_col_list ::= nm */
{
    uint32_t col = synq_parse_perfetto_per_column(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy381 = synq_parse_perfetto_per_column_list(pCtx, SYNTAQLITE_NULL_NODE, col);
}
  yymsp[0].minor.yy381 = yylhsminor.yy381;
        break;
      case 462: /* perfetto_per_col_list ::= perfetto_per_col_list COMMA nm */
{
    uint32_t col = synq_parse_perfetto_per_column(pCtx,
        synq_span_dequote(pCtx, yymsp[0].minor.yy0));
    yylhsminor.yy381 = synq_parse_perfetto_per_column_list(pCtx, yymsp[-2].minor.yy381, col);
}
  yymsp[-2].minor.yy381 = yylhsminor.yy381;
        break;
      case 464: /* perfetto_per ::= PER perfetto_per_col_list */
{ yymsp[-1].minor.yy381 = yymsp[0].minor.yy381; }
        break;
      case 465: /* perfetto_pipeline ::= FROM perfetto_pipe_source perfetto_pipe_stage_list */
{
    yymsp[-2].minor.yy381 = synq_parse_perfetto_pipeline(pCtx, yymsp[-1].minor.yy381, SYNTAQLITE_NULL_NODE, yymsp[0].minor.yy381);
}
        break;
      case 466: /* perfetto_pipeline ::= INTERVAL INTERSECTION OF LP perfetto_pipe_source_list RP perfetto_per perfetto_pipe_stage_list */
{
    uint32_t src = synq_parse_perfetto_interval_intersection(pCtx, yymsp[-3].minor.yy381, yymsp[-1].minor.yy381);
    yymsp[-7].minor.yy381 = synq_parse_perfetto_pipeline(pCtx, SYNTAQLITE_NULL_NODE, src, yymsp[0].minor.yy381);
}
        break;
      case 468: /* cmd ::= PERFETTO PRAGMA nm EQ expr */
{
    yymsp[-4].minor.yy381 = synq_parse_perfetto_pragma_stmt(pCtx, synq_span_dequote(pCtx, yymsp[-2].minor.yy0), yymsp[0].minor.yy381);
}
        break;
      case 469: /* cmd ::= CREATE perfetto_or_replace PERFETTO TABLE nm perfetto_table_impl perfetto_table_schema AS select_body_start select select_body_end */
{
    SyntaqliteTextSpan select_span = {
        .offset = yymsp[-2].minor.yy381,
        .length = yymsp[0].minor.yy381 - yymsp[-2].minor.yy381,
    };
    yymsp[-10].minor.yy381 = synq_parse_create_perfetto_table_stmt(pCtx,
        synq_span(pCtx, yymsp[-6].minor.yy0),
        yymsp[-9].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-5].minor.yy381, yymsp[-4].minor.yy381, yymsp[-1].minor.yy381, select_span, SYNTAQLITE_NULL_NODE);
}
        break;
      case 470: /* cmd ::= CREATE perfetto_or_replace PERFETTO TABLE nm perfetto_table_impl perfetto_table_schema AS perfetto_pipeline */
{
    yymsp[-8].minor.yy381 = synq_parse_create_perfetto_table_stmt(pCtx,
        synq_span(pCtx, yymsp[-4].minor.yy0),
        yymsp[-7].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-3].minor.yy381, yymsp[-2].minor.yy381, SYNTAQLITE_NULL_NODE, SYNQ_NO_SPAN, yymsp[0].minor.yy381);
}
        break;
      case 471: /* cmd ::= CREATE perfetto_or_replace PERFETTO VIEW nm perfetto_table_schema AS select_body_start select select_body_end */
{
    SyntaqliteTextSpan select_span = {
        .offset = yymsp[-2].minor.yy381,
        .length = yymsp[0].minor.yy381 - yymsp[-2].minor.yy381,
    };
    yymsp[-9].minor.yy381 = synq_parse_create_perfetto_view_stmt(pCtx,
        synq_span(pCtx, yymsp[-5].minor.yy0),
        yymsp[-8].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-4].minor.yy381, yymsp[-1].minor.yy381, select_span);
}
        break;
      case 472: /* cmd ::= CREATE perfetto_or_replace PERFETTO FUNCTION nm LP perfetto_arg_def_list RP RETURNS perfetto_return_type AS select_body_start select select_body_end */
{
    SyntaqliteTextSpan select_span = {
        .offset = yymsp[-2].minor.yy381,
        .length = yymsp[0].minor.yy381 - yymsp[-2].minor.yy381,
    };
    yymsp[-13].minor.yy381 = synq_parse_create_perfetto_function_stmt(pCtx,
        synq_span(pCtx, yymsp[-9].minor.yy0),
        yymsp[-12].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-7].minor.yy381, yymsp[-4].minor.yy381, yymsp[-1].minor.yy381, select_span);
}
        break;
      case 473: /* cmd ::= CREATE perfetto_or_replace PERFETTO FUNCTION nm LP perfetto_arg_def_list RP RETURNS perfetto_return_type DELEGATES TO ID */
{
    yymsp[-12].minor.yy381 = synq_parse_create_perfetto_delegating_function_stmt(pCtx,
        synq_span(pCtx, yymsp[-8].minor.yy0),
        yymsp[-11].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        yymsp[-6].minor.yy381, yymsp[-3].minor.yy381, synq_span(pCtx, yymsp[0].minor.yy0));
}
        break;
      case 474: /* cmd ::= CREATE perfetto_or_replace PERFETTO INDEX nm ON nm LP perfetto_indexed_col_list RP */
{
    yymsp[-9].minor.yy381 = synq_parse_create_perfetto_index_stmt(pCtx,
        synq_span(pCtx, yymsp[-5].minor.yy0),
        yymsp[-8].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        synq_span(pCtx, yymsp[-3].minor.yy0),
        yymsp[-1].minor.yy381);
}
        break;
      case 475: /* before_macro_body ::= */
{
    pCtx->in_macro_def_body++;
    yymsp[1].minor.yy686 = 0;
}
        break;
      case 477: /* perfetto_macro_body ::= perfetto_macro_body ANY */
{
    yylhsminor.yy0 = (SynqParseToken){
        .z = yymsp[-1].minor.yy0.z,
        .n = (uint32_t)(yymsp[0].minor.yy0.z + yymsp[0].minor.yy0.n - yymsp[-1].minor.yy0.z),
        .type = yymsp[-1].minor.yy0.type,
        .token_idx = yymsp[-1].minor.yy0.token_idx,
        .offset = yymsp[-1].minor.yy0.offset,
        .layer_id = yymsp[-1].minor.yy0.layer_id,
    };
}
  yymsp[-1].minor.yy0 = yylhsminor.yy0;
        break;
      case 478: /* cmd ::= CREATE perfetto_or_replace PERFETTO MACRO nm LP perfetto_macro_arg_list RP RETURNS ID before_macro_body AS perfetto_macro_body */
{
    synq_mark_as_type(pCtx, yymsp[-3].minor.yy0);
    if (pCtx->in_macro_def_body > 0) pCtx->in_macro_def_body--;
    yymsp[-12].minor.yy381 = synq_parse_create_perfetto_macro_stmt(pCtx,
        synq_span(pCtx, yymsp[-8].minor.yy0),
        yymsp[-11].minor.yy686 ? SYNTAQLITE_BOOL_TRUE : SYNTAQLITE_BOOL_FALSE,
        synq_span(pCtx, yymsp[-3].minor.yy0),
        synq_span(pCtx, yymsp[0].minor.yy0),
        yymsp[-6].minor.yy381);
}
        break;
      case 479: /* cmd ::= INCLUDE PERFETTO MODULE perfetto_module_name */
{
    yymsp[-3].minor.yy381 = synq_parse_include_perfetto_module_stmt(pCtx,
        synq_span(pCtx, yymsp[0].minor.yy0));
}
        break;
      case 480: /* cmd ::= DROP PERFETTO INDEX nm ON nm */
{
    yymsp[-5].minor.yy381 = synq_parse_drop_perfetto_index_stmt(pCtx,
        synq_span(pCtx, yymsp[-2].minor.yy0),
        synq_span(pCtx, yymsp[0].minor.yy0));
}
        break;
      default:
        break;
/********** End reduce actions ************************************************/
  };
  assert( yyruleno<sizeof(yyRuleInfoLhs)/sizeof(yyRuleInfoLhs[0]) );
  yygoto = yyRuleInfoLhs[yyruleno];
  yysize = yyRuleInfoNRhs[yyruleno];
  yyact = yy_find_reduce_action(yymsp[yysize].stateno,(YYCODETYPE)yygoto);

  /* There are no SHIFTREDUCE actions on nonterminals because the table
  ** generator has simplified them to pure REDUCE actions. */
  assert( !(yyact>YY_MAX_SHIFT && yyact<=YY_MAX_SHIFTREDUCE) );

  /* It is not possible for a REDUCE to be followed by an error */
  assert( yyact!=YY_ERROR_ACTION );

  yymsp += yysize+1;
  yypParser->yytos = yymsp;
  yymsp->stateno = (YYACTIONTYPE)yyact;
  yymsp->major = (YYCODETYPE)yygoto;
  yyTraceShift(yypParser, yyact, "... then shift");
  return yyact;
}

/*
** The following code executes when the parse fails
*/
#ifndef YYNOERRORRECOVERY
static void yy_parse_failed(
  yyParser *yypParser           /* The parser */
){
  SynqPerfettoParseARG_FETCH
  SynqPerfettoParseCTX_FETCH
#ifndef NDEBUG
  if( yyTraceFILE ){
    fprintf(yyTraceFILE,"%sFail!\n",yyTracePrompt);
  }
#endif
  while( yypParser->yytos>yypParser->yystack ) yy_pop_parser_stack(yypParser);
  /* Here code is inserted which will be executed whenever the
  ** parser fails */
/************ Begin %parse_failure code ***************************************/

    if (pCtx) {
        pCtx->error = 1;
    }
/************ End %parse_failure code *****************************************/
  SynqPerfettoParseARG_STORE /* Suppress warning about unused %extra_argument variable */
  SynqPerfettoParseCTX_STORE
}
#endif /* YYNOERRORRECOVERY */

/*
** The following code executes when a syntax error first occurs.
*/
static void yy_syntax_error(
  yyParser *yypParser,           /* The parser */
  int yymajor,                   /* The major type of the error token */
  SynqPerfettoParseTOKENTYPE yyminor         /* The minor type of the error token */
){
  SynqPerfettoParseARG_FETCH
  SynqPerfettoParseCTX_FETCH
#define TOKEN yyminor
/************ Begin %syntax_error code ****************************************/

  (void)yymajor;
  (void)TOKEN;
  if (pCtx) {
    pCtx->error = 1;
  }
/************ End %syntax_error code ******************************************/
  SynqPerfettoParseARG_STORE /* Suppress warning about unused %extra_argument variable */
  SynqPerfettoParseCTX_STORE
}

/*
** The following is executed when the parser accepts
*/
static void yy_accept(
  yyParser *yypParser           /* The parser */
){
  SynqPerfettoParseARG_FETCH
  SynqPerfettoParseCTX_FETCH
#ifndef NDEBUG
  if( yyTraceFILE ){
    fprintf(yyTraceFILE,"%sAccept!\n",yyTracePrompt);
  }
#endif
#ifndef YYNOERRORRECOVERY
  yypParser->yyerrcnt = -1;
#endif
  assert( yypParser->yytos==yypParser->yystack );
  /* Here code is inserted which will be executed whenever the
  ** parser accepts */
/*********** Begin %parse_accept code *****************************************/
/*********** End %parse_accept code *******************************************/
  SynqPerfettoParseARG_STORE /* Suppress warning about unused %extra_argument variable */
  SynqPerfettoParseCTX_STORE
}

/* The main parser program.
** The first argument is a pointer to a structure obtained from
** "SynqPerfettoParseAlloc" which describes the current state of the parser.
** The second argument is the major token number.  The third is
** the minor token.  The fourth optional argument is whatever the
** user wants (and specified in the grammar) and is available for
** use by the action routines.
**
** Inputs:
** <ul>
** <li> A pointer to the parser (an opaque structure.)
** <li> The major token number.
** <li> The minor token number.
** <li> An option argument of a grammar-specified type.
** </ul>
**
** Outputs:
** None.
*/
void SynqPerfettoParse(
  void *yyp,                   /* The parser */
  int yymajor,                 /* The major token code number */
  SynqPerfettoParseTOKENTYPE yyminor       /* The value for the token */
  SynqPerfettoParseARG_PDECL               /* Optional %extra_argument parameter */
){
  YYMINORTYPE yyminorunion;
  YYACTIONTYPE yyact;   /* The parser action. */
#if !defined(YYERRORSYMBOL) && !defined(YYNOERRORRECOVERY)
  int yyendofinput;     /* True if we are at the end of input */
#endif
#ifdef YYERRORSYMBOL
  int yyerrorhit = 0;   /* True if yymajor has invoked an error */
#endif
  yyParser *yypParser = (yyParser*)yyp;  /* The parser */
  SynqPerfettoParseCTX_FETCH
  SynqPerfettoParseARG_STORE

  assert( yypParser->yytos!=0 );
#if !defined(YYERRORSYMBOL) && !defined(YYNOERRORRECOVERY)
  yyendofinput = (yymajor==0);
#endif

  yyact = yypParser->yytos->stateno;
#ifndef NDEBUG
  if( yyTraceFILE ){
    if( yyact < YY_MIN_REDUCE ){
      fprintf(yyTraceFILE,"%sInput '%s' in state %d\n",
              yyTracePrompt,yyTokenName[yymajor],yyact);
    }else{
      fprintf(yyTraceFILE,"%sInput '%s' with pending reduce %d\n",
              yyTracePrompt,yyTokenName[yymajor],yyact-YY_MIN_REDUCE);
    }
  }
#endif

  while(1){ /* Exit by "break" */
    assert( yypParser->yytos>=yypParser->yystack );
    assert( yyact==yypParser->yytos->stateno );
    yyact = yy_find_shift_action((YYCODETYPE)yymajor,yyact);
    if( yyact >= YY_MIN_REDUCE ){
      unsigned int yyruleno = yyact - YY_MIN_REDUCE; /* Reduce by this rule */
#ifndef NDEBUG
      assert( yyruleno<(int)(sizeof(yyRuleName)/sizeof(yyRuleName[0])) );
      if( yyTraceFILE ){
        int yysize = yyRuleInfoNRhs[yyruleno];
        if( yysize ){
          fprintf(yyTraceFILE, "%sReduce %d [%s]%s, pop back to state %d.\n",
            yyTracePrompt,
            yyruleno, yyRuleName[yyruleno],
            yyruleno<YYNRULE_WITH_ACTION ? "" : " without external action",
            yypParser->yytos[yysize].stateno);
        }else{
          fprintf(yyTraceFILE, "%sReduce %d [%s]%s.\n",
            yyTracePrompt, yyruleno, yyRuleName[yyruleno],
            yyruleno<YYNRULE_WITH_ACTION ? "" : " without external action");
        }
      }
#endif /* NDEBUG */

      /* Check that the stack is large enough to grow by a single entry
      ** if the RHS of the rule is empty.  This ensures that there is room
      ** enough on the stack to push the LHS value */
      if( yyRuleInfoNRhs[yyruleno]==0 ){
#ifdef YYTRACKMAXSTACKDEPTH
        if( (int)(yypParser->yytos - yypParser->yystack)>yypParser->yyhwm ){
          yypParser->yyhwm++;
          assert( yypParser->yyhwm ==
                  (int)(yypParser->yytos - yypParser->yystack));
        }
#endif
        if( yypParser->yytos>=yypParser->yystackEnd ){
          if( yyGrowStack(yypParser) ){
            yyStackOverflow(yypParser);
            break;
          }
        }
      }
      yyact = yy_reduce(yypParser,yyruleno,yymajor,yyminor SynqPerfettoParseCTX_PARAM);
    }else if( yyact <= YY_MAX_SHIFTREDUCE ){
      yy_shift(yypParser,yyact,(YYCODETYPE)yymajor,yyminor);
#ifndef YYNOERRORRECOVERY
      yypParser->yyerrcnt--;
#endif
      break;
    }else if( yyact==YY_ACCEPT_ACTION ){
      yypParser->yytos--;
      yy_accept(yypParser);
      return;
    }else{
      assert( yyact == YY_ERROR_ACTION );
      yyminorunion.yy0 = yyminor;
#ifdef YYERRORSYMBOL
      int yymx;
#endif
#ifndef NDEBUG
      if( yyTraceFILE ){
        fprintf(yyTraceFILE,"%sSyntax Error!\n",yyTracePrompt);
      }
#endif
#ifdef YYERRORSYMBOL
      /* A syntax error has occurred.
      ** The response to an error depends upon whether or not the
      ** grammar defines an error token "ERROR".  
      **
      ** This is what we do if the grammar does define ERROR:
      **
      **  * Call the %syntax_error function.
      **
      **  * Begin popping the stack until we enter a state where
      **    it is legal to shift the error symbol, then shift
      **    the error symbol.
      **
      **  * Set the error count to three.
      **
      **  * Begin accepting and shifting new tokens.  No new error
      **    processing will occur until three tokens have been
      **    shifted successfully.
      **
      */
      if( yypParser->yyerrcnt<0 ){
        yy_syntax_error(yypParser,yymajor,yyminor);
      }
      yymx = yypParser->yytos->major;
      if( yymx==YYERRORSYMBOL || yyerrorhit ){
#ifndef NDEBUG
        if( yyTraceFILE ){
          fprintf(yyTraceFILE,"%sDiscard input token %s\n",
             yyTracePrompt,yyTokenName[yymajor]);
        }
#endif
        yy_destructor(yypParser, (YYCODETYPE)yymajor, &yyminorunion);
        yymajor = YYNOCODE;
      }else{
        while( yypParser->yytos > yypParser->yystack ){
          yyact = yy_find_reduce_action(yypParser->yytos->stateno,
                                        YYERRORSYMBOL);
          if( yyact<=YY_MAX_SHIFTREDUCE ) break;
          yy_pop_parser_stack(yypParser);
        }
        if( yypParser->yytos <= yypParser->yystack || yymajor==0 ){
          yy_destructor(yypParser,(YYCODETYPE)yymajor,&yyminorunion);
          yy_parse_failed(yypParser);
#ifndef YYNOERRORRECOVERY
          yypParser->yyerrcnt = -1;
#endif
          yymajor = YYNOCODE;
        }else if( yymx!=YYERRORSYMBOL ){
          yy_shift(yypParser,yyact,YYERRORSYMBOL,yyminor);
        }
      }
      yypParser->yyerrcnt = 3;
      yyerrorhit = 1;
      if( yymajor==YYNOCODE ) break;
      yyact = yypParser->yytos->stateno;
#elif defined(YYNOERRORRECOVERY)
      /* If the YYNOERRORRECOVERY macro is defined, then do not attempt to
      ** do any kind of error recovery.  Instead, simply invoke the syntax
      ** error routine and continue going as if nothing had happened.
      **
      ** Applications can set this macro (for example inside %include) if
      ** they intend to abandon the parse upon the first syntax error seen.
      */
      yy_syntax_error(yypParser,yymajor, yyminor);
      yy_destructor(yypParser,(YYCODETYPE)yymajor,&yyminorunion);
      break;
#else  /* YYERRORSYMBOL is not defined */
      /* This is what we do if the grammar does not define ERROR:
      **
      **  * Report an error message, and throw away the input token.
      **
      **  * If the input token is $, then fail the parse.
      **
      ** As before, subsequent error messages are suppressed until
      ** three input tokens have been successfully shifted.
      */
      if( yypParser->yyerrcnt<=0 ){
        yy_syntax_error(yypParser,yymajor, yyminor);
      }
      yypParser->yyerrcnt = 3;
      yy_destructor(yypParser,(YYCODETYPE)yymajor,&yyminorunion);
      if( yyendofinput ){
        yy_parse_failed(yypParser);
#ifndef YYNOERRORRECOVERY
        yypParser->yyerrcnt = -1;
#endif
      }
      break;
#endif
    }
  }
#ifndef NDEBUG
  if( yyTraceFILE ){
    yyStackEntry *i;
    char cDiv = '[';
    fprintf(yyTraceFILE,"%sReturn. Stack=",yyTracePrompt);
    for(i=&yypParser->yystack[1]; i<=yypParser->yytos; i++){
      fprintf(yyTraceFILE,"%c%s", cDiv, yyTokenName[i->major]);
      cDiv = ' ';
    }
    fprintf(yyTraceFILE,"]\n");
  }
#endif
  return;
}

/*
** Return the fallback token corresponding to canonical token iToken, or
** 0 if iToken has no fallback.
*/
int SynqPerfettoParseFallback(int iToken){
#ifdef YYFALLBACK
  assert( iToken<(int)(sizeof(yyFallback)/sizeof(yyFallback[0])) );
  return yyFallback[iToken];
#else
  (void)iToken;
  return 0;
#endif
}

/* syntaqlite extension: enumerate terminals that can be shifted/reduced from
** the parser's current state. Returns the total number of expected tokens,
** even when out_tokens/out_cap only request a prefix. */
static YYACTIONTYPE synq_find_reduce_action_safe(YYACTIONTYPE stateno, YYCODETYPE iLookAhead) {
int i;
if( stateno>YY_REDUCE_COUNT ) return yy_default[stateno];
i = yy_reduce_ofst[stateno] + iLookAhead;
if( i<0 || i>=YY_ACTTAB_COUNT || yy_lookahead[i]!=iLookAhead ) {
return yy_default[stateno];
}
return yy_action[i];
}

/* Like yy_find_shift_action but skips YYWILDCARD and YYFALLBACK paths.
** Wildcard matches are for error recovery (ANY token) and fallback matches
** accept keywords as identifiers — neither should appear as keyword
** autocompletion suggestions. */
static YYACTIONTYPE synq_find_shift_action_strict(
YYCODETYPE iLookAhead,
YYACTIONTYPE stateno
){
int i;
if( stateno>YY_MAX_SHIFT ) return stateno;
i = yy_shift_ofst[stateno];
assert( i>=0 );
assert( i+YYNTOKEN<=(int)YY_NLOOKAHEAD );
i += iLookAhead;
if( yy_lookahead[i]!=iLookAhead ){
/* No specific entry — skip fallback and wildcard, use default. */
return yy_default[stateno];
}
return yy_action[i];
}

static int synq_can_lookahead(yyParser* p, uint32_t token) {
YYACTIONTYPE stack_states[YYSTACKDEPTH + 1];
int top = 0;
int i = 0;
int steps = 0;

if( p==0 || p->yytos==0 ) return 0;

top = (int)(p->yytos - p->yystack);
if( top<0 || top>YYSTACKDEPTH ) return 0;
for(i=0; i<=top; i++) {
stack_states[i] = p->yystack[i].stateno;
}

while( steps++ < 10000 ) {
YYACTIONTYPE action = synq_find_shift_action_strict((YYCODETYPE)token, stack_states[top]);

if( action==YY_ERROR_ACTION || action==YY_NO_ACTION ) return 0;
if( action==YY_ACCEPT_ACTION ) return token==0;
if( action<=YY_MAX_SHIFT ) return 1;

/* Shift-reduce: the token is consumed (shifted) then a reduce follows.
** This means the token IS accepted, same as a pure shift. */
if( action>=YY_MIN_SHIFTREDUCE && action<=YY_MAX_SHIFTREDUCE ) return 1;

if( action>=YY_MIN_REDUCE && action<=YY_MAX_REDUCE ) {
int rule = (int)(action - YY_MIN_REDUCE);
int yysize = yyRuleInfoNRhs[rule];
YYACTIONTYPE goto_state;

top += yysize;  /* yyRuleInfoNRhs is negative rhs-size */
if( top<0 ) return 0;

goto_state = synq_find_reduce_action_safe(stack_states[top], yyRuleInfoLhs[rule]);
if( goto_state==YY_ERROR_ACTION || goto_state==YY_NO_ACTION ) return 0;

if( top>=YYSTACKDEPTH ) return 0;
top++;
stack_states[top] = goto_state;
continue;
}

return 0;
}

return 0;
}

uint32_t SynqPerfettoParseExpectedTokens(void* parser, uint32_t* out_tokens, uint32_t out_cap) {
uint32_t n = 0;
uint32_t token = 0;
yyParser* p = (yyParser*)parser;

if( p==0 || p->yytos==0 ) return 0;

for(token=1; token<YYNTOKEN; token++) {
if( !synq_can_lookahead(p, token) ) continue;
if( out_tokens && n<out_cap ) out_tokens[n] = token;
n++;
}

return n;
}

/* syntaqlite extension: non-terminal IDs for completion context. */
#define SYNQ_NT_INPUT 202
#define SYNQ_NT_CMDLIST 203
#define SYNQ_NT_ECMD 204
#define SYNQ_NT_CMDX 205
#define SYNQ_NT_ERROR 206
#define SYNQ_NT_CMD 207
#define SYNQ_NT_EXPR 208
#define SYNQ_NT_DISTINCT 209
#define SYNQ_NT_EXPRLIST 210
#define SYNQ_NT_SORTLIST 211
#define SYNQ_NT_FILTER_OVER 212
#define SYNQ_NT_TYPETOKEN 213
#define SYNQ_NT_TYPENAME 214
#define SYNQ_NT_SIGNED 215
#define SYNQ_NT_SELCOLLIST 216
#define SYNQ_NT_SCLP 217
#define SYNQ_NT_SCANPT 218
#define SYNQ_NT_NM 219
#define SYNQ_NT_MULTISELECT_OP 220
#define SYNQ_NT_IN_OP 221
#define SYNQ_NT_DBNM 222
#define SYNQ_NT_SELECTNOWITH 223
#define SYNQ_NT_ONESELECT 224
#define SYNQ_NT_SELECT 225
#define SYNQ_NT_PAREN_EXPRLIST 226
#define SYNQ_NT_LIKEOP 227
#define SYNQ_NT_BETWEEN_OP 228
#define SYNQ_NT_CASE_OPERAND 229
#define SYNQ_NT_CASE_EXPRLIST 230
#define SYNQ_NT_CASE_ELSE 231
#define SYNQ_NT_SCANTOK 232
#define SYNQ_NT_AUTOINC 233
#define SYNQ_NT_REFARGS 234
#define SYNQ_NT_REFARG 235
#define SYNQ_NT_REFACT 236
#define SYNQ_NT_DEFER_SUBCLAUSE 237
#define SYNQ_NT_INIT_DEFERRED_PRED_OPT 238
#define SYNQ_NT_DEFER_SUBCLAUSE_OPT 239
#define SYNQ_NT_TABLE_OPTION_SET 240
#define SYNQ_NT_TABLE_OPTION 241
#define SYNQ_NT_ONCONF 242
#define SYNQ_NT_CCONS 243
#define SYNQ_NT_CARGLIST 244
#define SYNQ_NT_TCONS 245
#define SYNQ_NT_CONSLIST 246
#define SYNQ_NT_TCONSCOMMA 247
#define SYNQ_NT_GENERATED 248
#define SYNQ_NT_CREATE_TABLE 249
#define SYNQ_NT_CREATE_TABLE_ARGS 250
#define SYNQ_NT_CREATEKW 251
#define SYNQ_NT_TEMP 252
#define SYNQ_NT_IFNOTEXISTS 253
#define SYNQ_NT_COLUMNLIST 254
#define SYNQ_NT_CONSLIST_OPT 255
#define SYNQ_NT_COLUMNNAME 256
#define SYNQ_NT_TERM 257
#define SYNQ_NT_SORTORDER 258
#define SYNQ_NT_EIDLIST_OPT 259
#define SYNQ_NT_EIDLIST 260
#define SYNQ_NT_RESOLVETYPE 261
#define SYNQ_NT_WITHNM 262
#define SYNQ_NT_WQAS 263
#define SYNQ_NT_COLLATE 264
#define SYNQ_NT_WQLIST 265
#define SYNQ_NT_WQITEM 266
#define SYNQ_NT_WITH 267
#define SYNQ_NT_INSERT_CMD 268
#define SYNQ_NT_ORCONF 269
#define SYNQ_NT_INDEXED_OPT 270
#define SYNQ_NT_WHERE_OPT_RET 271
#define SYNQ_NT_UPSERT 272
#define SYNQ_NT_RETURNING 273
#define SYNQ_NT_XFULLNAME 274
#define SYNQ_NT_ORDERBY_OPT 275
#define SYNQ_NT_LIMIT_OPT 276
#define SYNQ_NT_SETLIST 277
#define SYNQ_NT_FROM 278
#define SYNQ_NT_IDLIST_OPT 279
#define SYNQ_NT_RAISETYPE 280
#define SYNQ_NT_INDEXED_BY 281
#define SYNQ_NT_IDLIST 282
#define SYNQ_NT_WHERE_OPT 283
#define SYNQ_NT_NEXPRLIST 284
#define SYNQ_NT_NMORERR 285
#define SYNQ_NT_NULLS 286
#define SYNQ_NT_IFEXISTS 287
#define SYNQ_NT_TRANSTYPE 288
#define SYNQ_NT_TRANS_OPT 289
#define SYNQ_NT_SAVEPOINT_OPT 290
#define SYNQ_NT_KWCOLUMN_OPT 291
#define SYNQ_NT_FULLNAME 292
#define SYNQ_NT_ADD_COLUMN_FULLNAME 293
#define SYNQ_NT_AS 294
#define SYNQ_NT_GROUPBY_OPT 295
#define SYNQ_NT_HAVING_OPT 296
#define SYNQ_NT_WINDOW_CLAUSE 297
#define SYNQ_NT_SELTABLIST 298
#define SYNQ_NT_ON_USING 299
#define SYNQ_NT_JOINOP 300
#define SYNQ_NT_STL_PREFIX 301
#define SYNQ_NT_TRIGGER_TIME 302
#define SYNQ_NT_FOREACH_CLAUSE 303
#define SYNQ_NT_TRNM 304
#define SYNQ_NT_TRIGGER_DECL 305
#define SYNQ_NT_TRIGGER_CMD_LIST 306
#define SYNQ_NT_TRIGGER_EVENT 307
#define SYNQ_NT_WHEN_CLAUSE 308
#define SYNQ_NT_TRIGGER_CMD 309
#define SYNQ_NT_TRIDXBY 310
#define SYNQ_NT_DATABASE_KW_OPT 311
#define SYNQ_NT_PLUS_NUM 312
#define SYNQ_NT_MINUS_NUM 313
#define SYNQ_NT_NMNUM 314
#define SYNQ_NT_UNIQUEFLAG 315
#define SYNQ_NT_EXPLAIN 316
#define SYNQ_NT_KEY_OPT 317
#define SYNQ_NT_VINTO 318
#define SYNQ_NT_VALUES 319
#define SYNQ_NT_MVALUES 320
#define SYNQ_NT_CREATE_VTAB 321
#define SYNQ_NT_VTABARGLIST 322
#define SYNQ_NT_VTABARG 323
#define SYNQ_NT_VTABARGTOKEN 324
#define SYNQ_NT_LP 325
#define SYNQ_NT_ANYLIST 326
#define SYNQ_NT_RANGE_OR_ROWS 327
#define SYNQ_NT_FRAME_EXCLUDE_OPT 328
#define SYNQ_NT_FRAME_EXCLUDE 329
#define SYNQ_NT_WINDOWDEFN_LIST 330
#define SYNQ_NT_WINDOWDEFN 331
#define SYNQ_NT_WINDOW 332
#define SYNQ_NT_FRAME_OPT 333
#define SYNQ_NT_FRAME_BOUND_S 334
#define SYNQ_NT_FRAME_BOUND_E 335
#define SYNQ_NT_FRAME_BOUND 336
#define SYNQ_NT_FILTER_CLAUSE 337
#define SYNQ_NT_OVER_CLAUSE 338
#define SYNQ_NT_PERFETTO_OR_REPLACE 339
#define SYNQ_NT_PERFETTO_ARG_TYPE 340
#define SYNQ_NT_PERFETTO_ARG_DEF_LIST 341
#define SYNQ_NT_PERFETTO_ARG_DEF_LIST_NE 342
#define SYNQ_NT_PERFETTO_TABLE_SCHEMA 343
#define SYNQ_NT_PERFETTO_TABLE_IMPL 344
#define SYNQ_NT_PERFETTO_RETURN_TYPE 345
#define SYNQ_NT_PERFETTO_INDEXED_COL_LIST 346
#define SYNQ_NT_PERFETTO_MACRO_ARG_LIST 347
#define SYNQ_NT_PERFETTO_MACRO_ARG_LIST_NE 348
#define SYNQ_NT_PERFETTO_MODULE_NAME 349
#define SYNQ_NT_SELECT_BODY_START 350
#define SYNQ_NT_SELECT_BODY_END 351
#define SYNQ_NT_PERFETTO_PIPE 352
#define SYNQ_NT_PERFETTO_PIPE_SOURCE 353
#define SYNQ_NT_PERFETTO_TREE_DIRECTION 354
#define SYNQ_NT_PERFETTO_TREE_AGGREGATE 355
#define SYNQ_NT_PERFETTO_TREE_AGGREGATE_LIST 356
#define SYNQ_NT_PERFETTO_PIPE_SELECT_ITEM 357
#define SYNQ_NT_PERFETTO_PIPE_SELECT_LIST 358
#define SYNQ_NT_PERFETTO_PIPE_STAGE 359
#define SYNQ_NT_PERFETTO_PIPE_STAGE_LIST 360
#define SYNQ_NT_PERFETTO_PIPE_SOURCE_LIST 361
#define SYNQ_NT_PERFETTO_PER_COL_LIST 362
#define SYNQ_NT_PERFETTO_PER 363
#define SYNQ_NT_PERFETTO_PIPELINE 364
#define SYNQ_NT_BEFORE_MACRO_BODY 365
#define SYNQ_NT_PERFETTO_MACRO_BODY 366

/* syntaqlite extension: probe the goto table to check if a state has
** an explicit goto entry for non-terminal `nt`. */
static int synq_has_goto(YYACTIONTYPE state, YYCODETYPE nt) {
int i;
if( state>YY_REDUCE_COUNT ) return 0;
i = yy_reduce_ofst[state] + nt;
if( i<0 || i>=YY_ACTTAB_COUNT ) return 0;
return yy_lookahead[i] == nt;
}

/* syntaqlite extension: determine the semantic completion context
** (Expression vs TableRef) by walking the parser stack. Returns one of
** SYNTAQLITE_COMPLETION_CONTEXT_*. */
uint32_t SynqPerfettoParseCompletionContext(void* parser) {
yyParser* p = (yyParser*)parser;
if( p==0 || p->yytos==0 ) return SYNTAQLITE_COMPLETION_CONTEXT_UNKNOWN;

for(yyStackEntry* e = p->yytos; e >= p->yystack; e--) {
YYACTIONTYPE s = e->stateno;

/* Check if this state has gotos for table-ref non-terminals. */
if( synq_has_goto(s, SYNQ_NT_SELTABLIST)
|| synq_has_goto(s, SYNQ_NT_FULLNAME)
|| synq_has_goto(s, SYNQ_NT_XFULLNAME) ) {
return SYNTAQLITE_COMPLETION_CONTEXT_TABLE_REF;
}

/* Check if this state has gotos for expression non-terminals. */
if( synq_has_goto(s, SYNQ_NT_EXPR) ) {
return SYNTAQLITE_COMPLETION_CONTEXT_EXPRESSION;
}
}
return SYNTAQLITE_COMPLETION_CONTEXT_UNKNOWN;
}
/* ======== end: csrc/sqlite_parse.c ======== */

/* ======== begin: csrc/sqlite_keyword.h ======== */
#ifndef SYNTAQLITE_SQLITE_KEYWORD_H
#define SYNTAQLITE_SQLITE_KEYWORD_H
/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
** @generated by syntaqlite-buildtools — DO NOT EDIT
*/




int synq_sqlite3_keywordCode(const SyntaqliteDialect *env, const char* z, int n, int* pType);


#endif  /* SYNTAQLITE_SQLITE_KEYWORD_H */
/* ======== end: csrc/sqlite_keyword.h ======== */

/* ======== begin: csrc/sqlite_tokenize.c ======== */
/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
** @generated by syntaqlite-buildtools — DO NOT EDIT
*/



/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/
#define CC_X          0    /* The letter 'x', or start of BLOB literal */
#define CC_KYWD0      1    /* First letter of a keyword */
#define CC_KYWD       2    /* Alphabetics or '_'.  Usable in a keyword */
#define CC_DIGIT      3    /* Digits */
#define CC_DOLLAR     4    /* '$' */
#define CC_VARALPHA   5    /* '@', '#', ':'.  Alphabetic SQL variables */
#define CC_VARNUM     6    /* '?'.  Numeric SQL variables */
#define CC_SPACE      7    /* Space characters */
#define CC_QUOTE      8    /* '"', '\'', or '`'.  String literals, quoted ids */
#define CC_QUOTE2     9    /* '['.   [...] style quoted ids */
#define CC_PIPE      10    /* '|'.   Bitwise OR or concatenate */
#define CC_MINUS     11    /* '-'.  Minus or SQL-style comment */
#define CC_LT        12    /* '<'.  Part of < or <= or <> */
#define CC_GT        13    /* '>'.  Part of > or >= */
#define CC_EQ        14    /* '='.  Part of = or == */
#define CC_BANG      15    /* '!'.  Part of != */
#define CC_SLASH     16    /* '/'.  / or c-style comment */
#define CC_LP        17    /* '(' */
#define CC_RP        18    /* ')' */
#define CC_SEMI      19    /* ';' */
#define CC_PLUS      20    /* '+' */
#define CC_STAR      21    /* '*' */
#define CC_PERCENT   22    /* '%' */
#define CC_COMMA     23    /* ',' */
#define CC_AND       24    /* '&' */
#define CC_TILDA     25    /* '~' */
#define CC_DOT       26    /* '.' */
#define CC_ID        27    /* unicode characters usable in IDs */
#define CC_ILLEGAL   28    /* Illegal character */
#define CC_NUL       29    /* 0x00 */
#define CC_BOM       30    /* First byte of UTF8 BOM:  0xEF 0xBB 0xBF */

/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/
const unsigned char static sqlite3CtypeMap[256] = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 00..07    ........ */
  0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,  /* 08..0f    ........ */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 10..17    ........ */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 18..1f    ........ */
  0x01, 0x00, 0x80, 0x00, 0x40, 0x00, 0x00, 0x80,  /* 20..27     !"#$%&' */
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 28..2f    ()*+,-./ */
  0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c,  /* 30..37    01234567 */
  0x0c, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 38..3f    89:;<=>? */

  0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x02,  /* 40..47    @ABCDEFG */
  0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 48..4f    HIJKLMNO */
  0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,  /* 50..57    PQRSTUVW */
  0x02, 0x02, 0x02, 0x80, 0x00, 0x00, 0x00, 0x40,  /* 58..5f    XYZ[\]^_ */
  0x80, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x22,  /* 60..67    `abcdefg */
  0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,  /* 68..6f    hijklmno */
  0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,  /* 70..77    pqrstuvw */
  0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 78..7f    xyz{|}~. */

  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* 80..87    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* 88..8f    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* 90..97    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* 98..9f    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* a0..a7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* a8..af    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* b0..b7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* b8..bf    ........ */

  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* c0..c7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* c8..cf    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* d0..d7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* d8..df    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* e0..e7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* e8..ef    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,  /* f0..f7    ........ */
  0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40   /* f8..ff    ........ */
};

/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/
# define sqlite3Isspace(x)   (sqlite3CtypeMap[(unsigned char)(x)]&0x01)
# define sqlite3Isdigit(x)   (sqlite3CtypeMap[(unsigned char)(x)]&0x04)
# define sqlite3Isxdigit(x)  (sqlite3CtypeMap[(unsigned char)(x)]&0x08)

/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/
#ifdef SQLITE_ASCII
#define IdChar(C)  ((sqlite3CtypeMap[(unsigned char)C]&0x46)!=0)
#endif
#ifdef SQLITE_EBCDIC
const char sqlite3IsEbcdicIdChar[] = {
/* x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF */
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,  /* 4x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0,  /* 5x */
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0,  /* 6x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,  /* 7x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 0,  /* 8x */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 0, 1, 0,  /* 9x */
    1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 0,  /* Ax */
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  /* Bx */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1,  /* Cx */
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1,  /* Dx */
    0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1,  /* Ex */
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 0,  /* Fx */
};
#define IdChar(C)  (((c=C)>=0x42 && sqlite3IsEbcdicIdChar[c-0x40]))
#endif

/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/
static const unsigned char aiClass[] = {
#ifdef SQLITE_ASCII
/*         x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xa  xb  xc  xd  xe  xf */
/* 0x */   29, 28, 28, 28, 28, 28, 28, 28, 28,  7,  7, 28,  7,  7, 28, 28,
/* 1x */   28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 2x */    7, 15,  8,  5,  4, 22, 24,  8, 17, 18, 21, 20, 23, 11, 26, 16,
/* 3x */    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  5, 19, 12, 14, 13,  6,
/* 4x */    5,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
/* 5x */    1,  1,  1,  1,  1,  1,  1,  1,  0,  2,  2,  9, 28, 28, 28,  2,
/* 6x */    8,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1,
/* 7x */    1,  1,  1,  1,  1,  1,  1,  1,  0,  2,  2, 28, 10, 28, 25, 28,
/* 8x */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* 9x */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Ax */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Bx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Cx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Dx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
/* Ex */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 30,
/* Fx */   27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27
#endif
#ifdef SQLITE_EBCDIC
/*         x0  x1  x2  x3  x4  x5  x6  x7  x8  x9  xa  xb  xc  xd  xe  xf */
/* 0x */   29, 28, 28, 28, 28,  7, 28, 28, 28, 28, 28, 28,  7,  7, 28, 28,
/* 1x */   28, 28, 28, 28, 28,  7, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 2x */   28, 28, 28, 28, 28,  7, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 3x */   28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
/* 4x */    7, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 26, 12, 17, 20, 10,
/* 5x */   24, 28, 28, 28, 28, 28, 28, 28, 28, 28, 15,  4, 21, 18, 19, 28,
/* 6x */   11, 16, 28, 28, 28, 28, 28, 28, 28, 28, 28, 23, 22,  2, 13,  6,
/* 7x */   28, 28, 28, 28, 28, 28, 28, 28, 28,  8,  5,  5,  5,  8, 14,  8,
/* 8x */   28,  1,  1,  1,  1,  1,  1,  1,  1,  1, 28, 28, 28, 28, 28, 28,
/* 9x */   28,  1,  1,  1,  1,  1,  1,  1,  1,  1, 28, 28, 28, 28, 28, 28,
/* Ax */   28, 25,  1,  1,  1,  1,  1,  0,  2,  2, 28, 28, 28, 28, 28, 28,
/* Bx */   28, 28, 28, 28, 28, 28, 28, 28, 28, 28,  9, 28, 28, 28, 28, 28,
/* Cx */   28,  1,  1,  1,  1,  1,  1,  1,  1,  1, 28, 28, 28, 28, 28, 28,
/* Dx */   28,  1,  1,  1,  1,  1,  1,  1,  1,  1, 28, 28, 28, 28, 28, 28,
/* Ex */   28, 28,  1,  1,  1,  1,  1,  0,  2,  2, 28, 28, 28, 28, 28, 28,
/* Fx */    3,  3,  3,  3,  3,  3,  3,  3,  3,  3, 28, 28, 28, 28, 28, 28,
#endif
};

/*
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
*/
i64 SynqPerfettoGetToken(const SyntaqliteDialect* env, const unsigned char *z, int *tokenType){
  i64 i;
  int c;
  switch( aiClass[*z] ){  /* Switch on the character-class of the first byte
                          ** of the token. See the comment on the CC_ defines
                          ** above. */
    case CC_SPACE: {
      testcase( z[0]==' ' );
      testcase( z[0]=='\t' );
      testcase( z[0]=='\n' );
      testcase( z[0]=='\f' );
      testcase( z[0]=='\r' );
      for(i=1; sqlite3Isspace(z[i]); i++){}
      *tokenType = SYNTAQLITE_TK_SPACE;
      return i;
    }
    case CC_MINUS: {
      if( z[1]=='-' ){
        for(i=2; (c=z[i])!=0 && c!='\n'; i++){}
        *tokenType = SYNTAQLITE_TK_COMMENT;
        return i;
      }else if( z[1]=='>' ){
        *tokenType = SYNTAQLITE_TK_PTR;
        return 2 + (z[2]=='>');
      }
      *tokenType = SYNTAQLITE_TK_MINUS;
      return 1;
    }
    case CC_LP: {
      *tokenType = SYNTAQLITE_TK_LP;
      return 1;
    }
    case CC_RP: {
      *tokenType = SYNTAQLITE_TK_RP;
      return 1;
    }
    case CC_SEMI: {
      *tokenType = SYNTAQLITE_TK_SEMI;
      return 1;
    }
    case CC_PLUS: {
      *tokenType = SYNTAQLITE_TK_PLUS;
      return 1;
    }
    case CC_STAR: {
      *tokenType = SYNTAQLITE_TK_STAR;
      return 1;
    }
    case CC_SLASH: {
      if( z[1]!='*' || z[2]==0 ){
        *tokenType = SYNTAQLITE_TK_SLASH;
        return 1;
      }
      for(i=3, c=z[2]; (c!='*' || z[i]!='/') && (c=z[i])!=0; i++){}
      if( c ) i++;
      *tokenType = SYNTAQLITE_TK_COMMENT;
      return i;
    }
    case CC_PERCENT: {
      *tokenType = SYNTAQLITE_TK_REM;
      return 1;
    }
    case CC_EQ: {
      *tokenType = SYNTAQLITE_TK_EQ;
      return 1 + (z[1]=='=');
    }
    case CC_LT: {
      if( (c=z[1])=='=' ){
        *tokenType = SYNTAQLITE_TK_LE;
        return 2;
      }else if( c=='>' ){
        *tokenType = SYNTAQLITE_TK_NE;
        return 2;
      }else if( c=='<' ){
        *tokenType = SYNTAQLITE_TK_LSHIFT;
        return 2;
      }else{
        *tokenType = SYNTAQLITE_TK_LT;
        return 1;
      }
    }
    case CC_GT: {
      if( (c=z[1])=='=' ){
        *tokenType = SYNTAQLITE_TK_GE;
        return 2;
      }else if( c=='>' ){
        *tokenType = SYNTAQLITE_TK_RSHIFT;
        return 2;
      }else{
        *tokenType = SYNTAQLITE_TK_GT;
        return 1;
      }
    }
    case CC_BANG: {
      if( z[1]!='=' ){
        *tokenType = SYNTAQLITE_TK_ILLEGAL;
        return 1;
      }else{
        *tokenType = SYNTAQLITE_TK_NE;
        return 2;
      }
    }
    case CC_PIPE: {
      if( z[1]!='|' ){
        *tokenType = SYNTAQLITE_TK_BITOR;
        return 1;
      }else{
        *tokenType = SYNTAQLITE_TK_CONCAT;
        return 2;
      }
    }
    case CC_COMMA: {
      *tokenType = SYNTAQLITE_TK_COMMA;
      return 1;
    }
    case CC_AND: {
      *tokenType = SYNTAQLITE_TK_BITAND;
      return 1;
    }
    case CC_TILDA: {
      *tokenType = SYNTAQLITE_TK_BITNOT;
      return 1;
    }
    case CC_QUOTE: {
      int delim = z[0];
      testcase( delim=='`' );
      testcase( delim=='\'' );
      testcase( delim=='"' );
      for(i=1; (c=z[i])!=0; i++){
        if( c==delim ){
          if( z[i+1]==delim ){
            i++;
          }else{
            break;
          }
        }
      }
      if( c=='\'' ){
        *tokenType = SYNTAQLITE_TK_STRING;
        return i+1;
      }else if( c!=0 ){
        *tokenType = SYNTAQLITE_TK_ID;
        return i+1;
      }else{
        *tokenType = SYNTAQLITE_TK_ILLEGAL;
        return i;
      }
    }
    case CC_DOT: {
#ifndef SQLITE_OMIT_FLOATING_POINT
      if( !sqlite3Isdigit(z[1]) )
#endif
      {
        *tokenType = SYNTAQLITE_TK_DOT;
        return 1;
      }
      /* If the next character is a digit, this is a floating point
      ** number that begins with ".".  Fall thru into the next case */
      /* no break */ deliberate_fall_through
    }
    case CC_DIGIT: {
      testcase( z[0]=='0' );  testcase( z[0]=='1' );  testcase( z[0]=='2' );
      testcase( z[0]=='3' );  testcase( z[0]=='4' );  testcase( z[0]=='5' );
      testcase( z[0]=='6' );  testcase( z[0]=='7' );  testcase( z[0]=='8' );
      testcase( z[0]=='9' );  testcase( z[0]=='.' );
      *tokenType = SYNTAQLITE_TK_INTEGER;
#ifndef SQLITE_OMIT_HEX_INTEGER
      if( z[0]=='0' && (z[1]=='x' || z[1]=='X') && sqlite3Isxdigit(z[2]) ){
        for(i=3; 1; i++){
          if( sqlite3Isxdigit(z[i])==0 ){
            if( z[i]==SQLITE_DIGIT_SEPARATOR ){
              *tokenType = SYNTAQLITE_TK_QNUMBER;
            }else{
              break;
            }
          }
        }
      }else
#endif
        {
        for(i=0; 1; i++){
          if( sqlite3Isdigit(z[i])==0 ){
            if( z[i]==SQLITE_DIGIT_SEPARATOR ){
              *tokenType = SYNTAQLITE_TK_QNUMBER;
            }else{
              break;
            }
          }
        }
#ifndef SQLITE_OMIT_FLOATING_POINT
        if( z[i]=='.' ){
          if( *tokenType==SYNTAQLITE_TK_INTEGER ) *tokenType = SYNTAQLITE_TK_FLOAT;
          for(i++; 1; i++){
            if( sqlite3Isdigit(z[i])==0 ){
              if( z[i]==SQLITE_DIGIT_SEPARATOR ){
                *tokenType = SYNTAQLITE_TK_QNUMBER;
              }else{
                break;
              }
            }
          }
        }
        if( (z[i]=='e' || z[i]=='E') &&
             ( sqlite3Isdigit(z[i+1]) 
              || ((z[i+1]=='+' || z[i+1]=='-') && sqlite3Isdigit(z[i+2]))
             )
        ){
          if( *tokenType==SYNTAQLITE_TK_INTEGER ) *tokenType = SYNTAQLITE_TK_FLOAT;
          for(i+=2; 1; i++){
            if( sqlite3Isdigit(z[i])==0 ){
              if( z[i]==SQLITE_DIGIT_SEPARATOR ){
                *tokenType = SYNTAQLITE_TK_QNUMBER;
              }else{
                break;
              }
            }
          }
        }
#endif
      }
      while( IdChar(z[i]) ){
        *tokenType = SYNTAQLITE_TK_ILLEGAL;
        i++;
      }
      return i;
    }
    case CC_QUOTE2: {
      for(i=1, c=z[0]; c!=']' && (c=z[i])!=0; i++){}
      *tokenType = c==']' ? SYNTAQLITE_TK_ID : SYNTAQLITE_TK_ILLEGAL;
      return i;
    }
    case CC_VARNUM: {
      *tokenType = SYNTAQLITE_TK_VARIABLE;
      for(i=1; sqlite3Isdigit(z[i]); i++){}
      return i;
    }
    case CC_DOLLAR:
    case CC_VARALPHA: {
      int n = 0;
      testcase( z[0]=='$' );  testcase( z[0]=='@' );
      testcase( z[0]==':' );  testcase( z[0]=='#' );
      *tokenType = SYNTAQLITE_TK_VARIABLE;
      for(i=1; (c=z[i])!=0; i++){
        if( IdChar(c) ){
          n++;
#ifndef SQLITE_OMIT_TCL_VARIABLE
        }else if( c=='(' && n>0 ){
          do{
            i++;
          }while( (c=z[i])!=0 && !sqlite3Isspace(c) && c!=')' );
          if( c==')' ){
            i++;
          }else{
            *tokenType = SYNTAQLITE_TK_ILLEGAL;
          }
          break;
        }else if( c==':' && z[i+1]==':' ){
          i++;
#endif
        }else{
          break;
        }
      }
      if( n==0 ) *tokenType = SYNTAQLITE_TK_ILLEGAL;
      return i;
    }
    case CC_KYWD0: {
      if( aiClass[z[1]]>CC_KYWD ){ i = 1;  break; }
      for(i=2; aiClass[z[i]]<=CC_KYWD; i++){}
      if( IdChar(z[i]) ){
        /* This token started out using characters that can appear in keywords,
        ** but z[i] is a character not allowed within keywords, so this must
        ** be an identifier instead */
        i++;
        break;
      }
      *tokenType = SYNTAQLITE_TK_ID;
      return synq_sqlite3_keywordCode(env, (char*)z, i, tokenType);
    }
    case CC_X: {
#ifndef SQLITE_OMIT_BLOB_LITERAL
      testcase( z[0]=='x' ); testcase( z[0]=='X' );
      if( z[1]=='\'' ){
        *tokenType = SYNTAQLITE_TK_BLOB;
        for(i=2; sqlite3Isxdigit(z[i]); i++){}
        if( z[i]!='\'' || i%2 ){
          *tokenType = SYNTAQLITE_TK_ILLEGAL;
          while( z[i] && z[i]!='\'' ){ i++; }
        }
        if( z[i] ) i++;
        return i;
      }
#endif
      /* If it is not a BLOB literal, then it must be an ID, since no
      ** SQL keywords start with the letter 'x'.  Fall through */
      /* no break */ deliberate_fall_through
    }
    case CC_KYWD:
    case CC_ID: {
      i = 1;
      break;
    }
    case CC_BOM: {
      if( z[1]==0xbb && z[2]==0xbf ){
        *tokenType = SYNTAQLITE_TK_SPACE;
        return 3;
      }
      i = 1;
      break;
    }
    case CC_NUL: {
      *tokenType = SYNTAQLITE_TK_ILLEGAL;
      return 0;
    }
    default: {
      *tokenType = SYNTAQLITE_TK_ILLEGAL;
      return 1;
    }
  }
  while( IdChar(z[i]) ){ i++; }
  *tokenType = SYNTAQLITE_TK_ID;
  return i;
}
/* ======== end: csrc/sqlite_tokenize.c ======== */

/* ======== begin: csrc/token_wrapped.c ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.

// Version-compatibility wrapper for the SQLite tokenizer.
//
// Wraps SynqSqliteGetToken and reclassifies tokens that were introduced
// in newer SQLite versions.  When SQLite adds new token types in future
// versions, add a new version gate here following the existing pattern.


int64_t SynqSqliteGetTokenVersionWrapped(const SyntaqliteDialect* g,
                                         uint32_t macro_fallback,
                                         const unsigned char* z,
                                         uint32_t* tokenType) {
  int token_type_int = 0;
  int64_t len = SYNQ_GET_TOKEN(g, z, &token_type_int);
  *tokenType = (uint32_t)token_type_int;

#ifndef SYNTAQLITE_OMIT_MACROS
  if (*tokenType == SYNTAQLITE_TK_ILLEGAL && len == 1 && z[0] == '!' &&
      (g->tmpl->macro_style == SYNQ_MACRO_STYLE_RUST || macro_fallback)) {
    *tokenType = SYNTAQLITE_TK_BANG;
    return 1;
  }
#endif

  if (SYNQ_VER_LT(g, 3038000) && *tokenType == SYNTAQLITE_TK_PTR) {
    /* -> and ->> operators added in 3.38.
    ** Return just the '-' as TK_MINUS; next call picks up '>' naturally. */
    *tokenType = SYNTAQLITE_TK_MINUS;
    return 1;
  }

  if (SYNQ_VER_LT(g, 3046000) && *tokenType == SYNTAQLITE_TK_QNUMBER) {
    /* Digit separators added in 3.46.
    ** Truncate to the first underscore. */
    int64_t j;
    int saw_dot = 0;
    for (j = 0; j < len; j++) {
      if (z[j] == '_')
        break;
      if (z[j] == '.')
        saw_dot = 1;
    }
    *tokenType = saw_dot ? SYNTAQLITE_TK_FLOAT : SYNTAQLITE_TK_INTEGER;
    return j;
  }

  return len;
}
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/token_wrapped.c ======== */

/* ======== begin: csrc/tokenizer.c ======== */
#ifndef SYNTAQLITE_OMIT_RUNTIME
// Copyright 2025 The syntaqlite Authors. All rights reserved.
// Licensed under the Apache License, Version 2.0.


#include <string.h>


struct SyntaqliteTokenizer {
  SyntaqliteMemMethods mem;
  SyntaqliteDialect env;
  const char* source;
  uint32_t len;
  uint32_t offset;
};

SYNTAQLITE_API SyntaqliteTokenizer* syntaqlite_tokenizer_create_with_dialect(
    const SyntaqliteMemMethods* mem,
    const SyntaqliteDialect env) {
  SyntaqliteMemMethods m = mem ? *mem : SYNTAQLITE_MEM_METHODS_DEFAULT;
  SyntaqliteTokenizer* tok = m.xMalloc(sizeof(SyntaqliteTokenizer));
  memset(tok, 0, sizeof(*tok));
  tok->mem = m;
  tok->env = env;
  return tok;
}

#ifndef SYNTAQLITE_OMIT_SQLITE_API
SYNTAQLITE_API SyntaqliteTokenizer* syntaqlite_tokenizer_create(
    const SyntaqliteMemMethods* mem) {
  SyntaqliteDialect env = syntaqlite_sqlite_dialect();
  return syntaqlite_tokenizer_create_with_dialect(mem, env);
}
#endif

SYNTAQLITE_API void syntaqlite_tokenizer_reset(SyntaqliteTokenizer* tok,
                                               const char* source,
                                               uint32_t len) {
  tok->source = source;
  tok->len = len;
  tok->offset = 0;
}

SYNTAQLITE_API uint32_t syntaqlite_tokenizer_next(SyntaqliteTokenizer* tok,
                                                  SyntaqliteToken* out) {
  if (tok->offset >= tok->len) {
    return 0;
  }

  uint32_t token_type = 0;
  // Standalone tokenizer has no parser context, so macro_fallback is 0.
  int64_t token_len = SynqSqliteGetTokenVersionWrapped(
      &tok->env, 0, (const unsigned char*)tok->source + tok->offset,
      &token_type);

  out->text = tok->source + tok->offset;
  out->length = (uint32_t)token_len;
  out->type = token_type;

  tok->offset += (uint32_t)token_len;
  return 1;
}

SYNTAQLITE_API void syntaqlite_tokenizer_destroy(SyntaqliteTokenizer* tok) {
  if (tok) {
    tok->mem.xFree(tok);
  }
}
#endif /* !SYNTAQLITE_OMIT_RUNTIME */
/* ======== end: csrc/tokenizer.c ======== */


#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
