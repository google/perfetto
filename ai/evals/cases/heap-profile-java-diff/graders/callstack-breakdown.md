---
type: regex
pattern: "45,008,848|44,861,464|45\\.0 MB|18,326,792|18\\.3 MB"
---
Ground truth: `TileIconRenderer.rasterizeDynamicVectorLayers` (+63,335,640 B)
splits across two callstacks: `TileIconRenderer$IconLayerBuffer.<init>`
(45,008,848 B / +44,861,464 B / ~45.0 MB) and direct allocations in
`rasterizeDynamicVectorLayers` (18,326,792 B / ~18.3 MB).
