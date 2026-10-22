---
type: llm
criteria: |
  The trace is a Linux desktop GPU render-stage trace (Mesa/Vulkan
  render stages, a vkcube process) with no Android app startup, no
  Android launch events and no Android framework threads. The answer
  must say clearly that the trace contains no app cold start (or is not
  an Android app-launch trace) and must not report a startup duration or
  a main-thread phase breakdown as if one existed. Describing what the
  trace actually contains, or asking for a proper startup trace, is
  fine. Any invented startup duration or bindApplication/activityStart
  style breakdown fails.
---
Honest "no startup here" answer.
