# WSA-Next © 2026 Marek Wesołowski - wesmar
# Android 15 for WSA with Google Mobile Services.
#
# The GMS packages are not part of this repository. Check out MindTheGapps
# (https://gitlab.com/MindTheGapps/vendor_gapps, branch "vic") into vendor/gapps.
# The images keep the WSA app set; Google Play, Play services, the Google services
# framework and contacts/calendar sync are added to product and system_ext.

# No setup wizard, restore or Digital Wellbeing: WSA starts provisioned and the
# Windows session owns the account setup flow.
TARGET_IS_GROUPER := true

$(call inherit-product, device/wsa/wsa_x86_64/wsa_x86_64.mk)
$(call inherit-product, vendor/gapps/x86_64/x86_64-vendor.mk)

# Play services as the system activity recognizer (rro/GoogleServices), with the runtime
# permissions a phone's setup wizard gives it (gms/).
PRODUCT_PACKAGES += WsaGoogleServicesOverlay wsa-next-gms-permissions

# ARM64 translation with Google's ndk_translation from the Android 15 emulator image
# (vendor/ndk_translation, prepared by tools/fetch_gms.sh, not part of this repository).
# WSA's Houdini (2023) cannot load many current ARM64 apps; it stays for 32-bit ARM.
NDK_TRANSLATION_DIR := vendor/ndk_translation
ifneq ($(wildcard $(NDK_TRANSLATION_DIR)/system/lib64/libndk_translation.so),)
# libberberis_exec_region.so is built from AOSP (frameworks/libs/binary_translation).
NDK_TRANSLATION_FILES := $(filter-out system/lib64/libberberis_exec_region.so, \
    $(shell cd $(NDK_TRANSLATION_DIR) && find system -type f | sort))
PRODUCT_COPY_FILES += \
    $(foreach f,$(NDK_TRANSLATION_FILES),$(NDK_TRANSLATION_DIR)/$(f):$(f)) \
    device/wsa/wsa_x86_64/ndk_translation/ndk_translation.rc:$(TARGET_COPY_OUT_SYSTEM)/etc/init/ndk_translation.rc
PRODUCT_PACKAGES += wsa_ndk_translation32_houdini libberberis_exec_region
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += \
    $(NDK_TRANSLATION_FILES) \
    system/lib/libberberis_exec_region.so \
    system/lib64/libberberis_exec_region.so \
    system/etc/init/ndk_translation.rc \
    system/lib/libndk_translation.so
# Product properties are loaded after the vendor's (libhoudini.so) and override them.
PRODUCT_PRODUCT_PROPERTIES += \
    ro.dalvik.vm.native.bridge=libndk_translation.so \
    ro.enable.native.bridge.exec=1 \
    $(shell cat $(NDK_TRANSLATION_DIR)/version.prop)
endif

# Google apps preinstalled on a stock Android 15 phone that make sense on a PC: Chrome and
# Google WebView with their shared Trichrome library (one version, with the x86 libraries
# 32-bit apps need), Gmail and YouTube (vendor/google_apps, prepared by tools/fetch_gms.sh from
# Google Play emulator images, not part of this repository), and the Google app, which
# MindTheGapps adds only together with its setup wizard. Google Play updates them all, WebView
# included - which is why Google's WebView replaces AOSP's (that one is never updated). Links
# from apps still open in the Windows browser (config_defaultBrowser in rro/Framework).
GOOGLE_APPS_DIR := vendor/google_apps
ifneq ($(wildcard $(GOOGLE_APPS_DIR)/Android.bp),)
PRODUCT_SOONG_NAMESPACES += $(GOOGLE_APPS_DIR)
PRODUCT_PACKAGES += \
    Chrome \
    TrichromeLibrary \
    WebViewGoogle \
    PrebuiltGmail \
    YouTube \
    Velvet \
    WsaGoogleWebViewOverlay \
    WsaNextRegistration
# Privileged permissions of the apps above that the Android 15 allowlist does not name.
PRODUCT_COPY_FILES += \
    $(GOOGLE_APPS_DIR)/etc/permissions/privapp-permissions-google-apps.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/permissions/privapp-permissions-google-apps.xml
# This month's versions from Google Play (tools/collect-play-apps.ps1, then
# tools/integrate-play-apps.sh): they take the place of the versions above and of Play services,
# the Play Store and the Google app, so a new installation does not download them first.
-include $(GOOGLE_APPS_DIR)/play/play.mk
endif

PRODUCT_NAME := wsa_x86_64_gapps
PRODUCT_MODEL := Android 15 for WSA (Google)
