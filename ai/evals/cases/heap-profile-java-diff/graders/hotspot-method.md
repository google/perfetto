---
type: regex
pattern: "rasterizeDynamicVectorLayers|TileIconRenderer"
---
Ground truth: `com.android.systemui.qs.panel.TileIconRenderer.rasterizeDynamicVectorLayers`
(and `IconLayerBuffer.<init>`) is the Java method responsible for the Java heap
allocation regression (+63.20 MB, +14,832 allocations).
