# Preserve JNI entry points for Homography Auto-Mosaic native bridge
-keepclasseswithmembernames class * {
    native <methods>;
}
-keep class com.antigravity.homographymosaic.core.MosaicNative { *; }
