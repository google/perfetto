---
type: regex
pattern: "compileRuntimeBlurKernel|BlurShaderEngine"
---
Ground truth: native JNI function
`Java_com_android_systemui_qs_panel_BlurShaderEngine_compileRuntimeBlurKernel`
(called from `com.android.systemui.qs.panel.BlurShaderEngine.updateBlurFrame`)
is the root cause of the native allocation regression.
