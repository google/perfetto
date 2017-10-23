LLVM sanitizers smoke tests
---------------------------
The only purpose of this binary is to verify that the various build configs for
sanitizers (`is_asan`, `is_lsan`, `is_msan`, `is_tsan`, `is_ubsan`) do actually
work and spot violations, rather than unconditionally succeeding.
All the test fixtures in `sanitizers_unittests` are expected to fail under their
corresponding sanitizer configuration. A green test means failure here.
