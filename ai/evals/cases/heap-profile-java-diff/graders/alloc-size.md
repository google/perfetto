---
type: regex
pattern: "63[.,][1234]\\s*MB|66,?432,?000|66,?276,?864|14,?8[36][0-9]"
---
Ground truth: `com.android.art` allocations increased from ~151.5 KB (36
allocations) in the before trace to ~63.36 MB (14,868 allocations) in the after
trace (+63.20 MB, +14,832 allocations).
