---
type: llm
criteria: |
  The answer lists binder transactions made by Gmail's main thread during
  the cold start with per-call or per-interface durations taken from the
  trace, names IActivityManager.attachApplication (about 12 ms, handled by
  system_server) as the single most expensive call, and mentions at least
  two other interfaces from this set: servicemanager lookups (~22 calls,
  ~8 ms total), IJobScheduler.enqueue (~6 ms), IActivityTaskManager.startActivity
  (~5 ms), IActivityManager.isUserAMonkey (~3 ms), IWindowSession.relayout
  (~3 ms). Interface names may be abbreviated. An answer with no numbers,
  or with a different top call, fails.
---
Grounded ranking of binder calls.
