---
type: llm
criteria: |
  The answer reports a concrete cold-start duration for Gmail
  (com.google.android.gm) of roughly 379 ms, and attributes the main
  thread time to concrete named phases or slices taken from the trace
  (for example bindApplication ~137 ms and activityStart ~99 ms with
  performCreate of ConversationListActivityGmail inside it). It must not
  present guesses or generic Android startup advice as if they were
  measurements from this trace. Generic advice is allowed only if clearly
  separated from the measured findings.
---
Grounded, non-fabricated answer.
