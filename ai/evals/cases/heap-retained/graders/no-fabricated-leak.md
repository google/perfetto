---
type: llm
criteria: |
  The answer identifies what retains the most memory using numbers from
  the heap dump (interned java.lang.String ~2.4 MB, and/or Android ICU data such as
  ICUResourceBundleReader caches, ~2-6 MB retained depending on grouping;
  reachable heap ~27-28 MB of a ~35 MB system_server dump), and it is honest
  about leak evidence: the top retainers are interned strings and ICU
  resource caches, which are not a clear leak, so it should not assert a
  definite leak. An answer that confidently declares a specific leak without
  trace-derived evidence, or that invents classes not in the dump, fails.
---
Grounded and honest about weak leak evidence.
