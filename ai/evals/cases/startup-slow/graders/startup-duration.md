---
type: regex
pattern: "37[0-9](\\.\\d+)?\\s*ms|38[0-9](\\.\\d+)?\\s*ms|0\\.3[78]\\d*\\s*s"
---
Ground truth from android.startup.startups: com.google.android.gm cold
start, 378.6 ms. (The trace also has a photos cold start of 167.6 ms and a
vending cold start of 287.5 ms; those must not be reported as Gmail's.)
