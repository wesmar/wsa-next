# WSA-Next © 2026 Marek Wesołowski - wesmar
# Android 15 system images for the Windows Subsystem for Android 2407 host.
#
# Only system, system_ext and product are built. The vendor partition, kernel and
# initrd come from the WSA package, so the layout must match WSA's vendor fstab:
#   system      root filesystem
#   system_ext  separate disk (SCSI 0:0:0:2), mounted by first-stage init
#   product     separate disk (SCSI 0:0:0:3), mounted by first-stage init

DEVICE_PATH := device/wsa/wsa_x86_64

PRODUCT_USE_DYNAMIC_PARTITIONS := false

# Generic 64/32-bit system image. WSA for Android 13 shipped the same kind of
# generic ("mainline") system, with all host integration living elsewhere.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/generic_system.mk)
PRODUCT_ENFORCE_ARTIFACT_PATH_REQUIREMENTS := true

# system_ext and product carry the same app set as WSA 2407, not the phone set:
# no launcher (WSA HomeApp), no on-screen keyboard (WSA's MicrosoftInputMethod),
# no dialer, messaging, clock, calendar, browser or system sounds.
$(call inherit-product, $(SRC_TARGET_DIR)/product/media_system_ext.mk)
PRODUCT_PACKAGES += \
    Provision \
    Settings \
    SystemUI

$(call inherit-product, $(SRC_TARGET_DIR)/product/media_product.mk)
PRODUCT_PACKAGES += \
    Camera2 \
    Contacts \
    Gallery2 \
    Music \
    SettingsIntelligence \
    frameworks-base-overlays \
    preinstalled-packages-platform-handheld-product.xml

# WSA integration layer (system_server services, SystemUI variant, Microsoft apps).
# Lives outside this repository; without it the build is plain Android 15.
$(call inherit-product-if-exists, vendor/wsa/wsa-vendor.mk)

# First-stage mount tables. WSA has no boot ramdisk: its initrd only mounts the system
# disk and starts /init, which reads /system/etc/fstab.${androidboot.fstab_suffix}.
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/fstab.1.windows:$(TARGET_COPY_OUT_SYSTEM)/etc/fstab.1.windows \
    $(DEVICE_PATH)/fstab.2.windows:$(TARGET_COPY_OUT_SYSTEM)/etc/fstab.2.windows
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += \
    system/etc/fstab.1.windows \
    system/etc/fstab.2.windows

# ARM translation: /system paths expected by the Houdini setup in WSA's vendor partition
PRODUCT_PACKAGES += \
    wsa_libhoudini_symlink \
    wsa_libhoudini64_symlink \
    wsa_houdini_mountpoint \
    wsa_houdini64_mountpoint
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += \
    system/bin/houdini \
    system/bin/houdini64 \
    system/lib/libhoudini.so \
    system/lib64/libhoudini.so

# Configuration overlays matching WSA 2407 defaults
PRODUCT_PACKAGES += \
    WsaDocumentsUIOverlay \
    WsaFrameworkOverlay \
    WsaNetworkStackOverlay \
    WsaSettingsProviderOverlay

# Bluetooth (WSA 2407 had none): an HCI HAL that uses the PC's adapter through the Windows
# Bluetooth bridge (device/wsa/wsa_x86_64/bluetooth).
PRODUCT_PACKAGES += \
    android.hardware.bluetooth-service.wsa

# Fingerprint (WSA 2407 had none): an AIDL IFingerprint HAL backed by Windows Hello on the host
# over vsock port "WSFP" (device/wsa/wsa_x86_64/fingerprint).
PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint-service.wsa
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/permissions/android.hardware.fingerprint.xml

# SIM, network and SMS (WSA 2407 had none) from the PC's modem: Cuttlefish's AT-command RIL as a
# platform HAL on vsock port "WSRL", answered by the modem bridge on Windows
# (device/wsa/wsa_x86_64/telephony, android/patches/device/google/cuttlefish). Telephony, its
# provider, MMS and SIM toolkit come with generic_system; carrier settings are added here. The
# radio HAL's libraries go to /system like the radio@1.0-1.4 ones generic_system already ships.
PRODUCT_PACKAGES += \
    libcuttlefish-rild \
    libcuttlefish-ril-2 \
    wsa-ril.rc \
    wsa-ril-manifest.xml \
    wsa-telephony-features.xml \
    CarrierConfig
PRODUCT_ARTIFACT_PATH_REQUIREMENT_ALLOWED_LIST += \
    system/lib64/android.hardware.radio%

# MMS through the PC's modem: Android has no mobile data, so MmsService (patched,
# android/patches/packages/services/Mms) hands each exchange to the wsa-mms daemon, and WSA-Next
# on Windows opens the operator's MMS APN on the modem. The operators' access points (AOSP's
# list, as Cuttlefish ships it) give the MMS server.
PRODUCT_PACKAGES += \
    wsa-mms
PRODUCT_COPY_FILES += \
    device/sample/etc/apns-full-conf.xml:$(TARGET_COPY_OUT_PRODUCT)/etc/apns-conf.xml
# No mobile data in Android: the PC has its own network. Off by default; the RIL refuses data
# calls for good if it is turned on (android/patches/device/google/cuttlefish/0002).
PRODUCT_SYSTEM_PROPERTIES += ro.com.android.mobiledata=false

# adb lets in only computers the user allowed ("Allow debugging?"), as on a user build. AOSP
# sets this on user builds only, and on this debuggable build it matters more: Microsoft's vendor
# init starts adbd on every debuggable build whatever WSA's developer mode says, WsaClient
# forwards port 58526 on all addresses, and Microsoft's package opens that port to the private
# network - without it, any computer on a home network had a root shell (adb root).
PRODUCT_SYSTEM_PROPERTIES += ro.adb.secure=1

# Device-wide init actions of WSA-Next (the serial console shell of debuggable builds is stopped).
PRODUCT_COPY_FILES += \
    device/wsa/wsa_x86_64/init/init.wsa-next.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.wsa-next.rc

# Root for apps instead of Magisk: "0" asks, Windows Hello on the PC confirms every request,
# the images stay unchanged (device/wsa/wsa_x86_64/elevate). Off until the user turns it on in
# WSA-Next on Windows. Debug builds only: the elevated domain exists only there.
PRODUCT_PACKAGES_DEBUG += \
    wsa-elevated \
    wsa-zero \
    wsa-elevate-exec

# App data backup (WSA 2407 had none): the backup service only starts when the device
# declares the feature.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.software.backup.xml:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/permissions/android.software.backup.xml

PRODUCT_BUILD_SYSTEM_IMAGE := true
PRODUCT_BUILD_SYSTEM_EXT_IMAGE := true
PRODUCT_BUILD_PRODUCT_IMAGE := true
PRODUCT_BUILD_VENDOR_IMAGE := false
PRODUCT_BUILD_ODM_IMAGE := false
PRODUCT_BUILD_VENDOR_DLKM_IMAGE := false
PRODUCT_BUILD_ODM_DLKM_IMAGE := false
PRODUCT_BUILD_SYSTEM_DLKM_IMAGE := false
PRODUCT_BUILD_RAMDISK_IMAGE := false
PRODUCT_BUILD_BOOT_IMAGE := false
PRODUCT_BUILD_INIT_BOOT_IMAGE := false
PRODUCT_BUILD_VENDOR_BOOT_IMAGE := false
PRODUCT_BUILD_RECOVERY_IMAGE := false
PRODUCT_BUILD_USERDATA_IMAGE := false
PRODUCT_BUILD_CACHE_IMAGE := false
PRODUCT_BUILD_SYSTEM_OTHER_IMAGE := false

# The reused WSA vendor partition is Android 13 with first API level 30.
PRODUCT_SHIPPING_API_LEVEL := 30

# Runtime behaviour of WSA 2407 (system_ext/product build.prop there):
#  - no RescueParty factory-reset loops on a desktop host,
#  - privileged permission allowlist violations are logged instead of fatal,
#  - no predictive back gesture (windows are closed by Windows),
#  - FUSE passthrough for emulated storage performance.
# WSA 2407 also disabled shell transitions; Android 15 ignores that switch outside
# automotive, so the WSA shell runs with transitions enabled.
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    persist.sys.disable_rescue=true \
    ro.control_privapp_permissions=log \
    persist.wm.debug.predictive_back=0
PRODUCT_PRODUCT_PROPERTIES += \
    persist.sys.fuse.passthrough.enable=true

# The WSA vendor partition links against VNDK 33 (ro.vndk.version=33). Android 15
# no longer builds a VNDK of its own, so ship the v33 snapshot like the GSI does.
PRODUCT_EXTRA_VNDK_VERSIONS := 33

# Builds are signed with the project's keys from vendor/wsa-keys when that folder exists
# (it is not part of this repository; userdebug included, as releases are userdebug for
# now). Without it they keep the AOSP test keys. Changing the platform key requires a
# clean install.
ifneq ($(wildcard vendor/wsa-keys/releasekey.x509.pem),)
PRODUCT_DEFAULT_DEV_CERTIFICATE := vendor/wsa-keys/releasekey
endif

PRODUCT_NAME := wsa_x86_64
PRODUCT_DEVICE := wsa_x86_64
PRODUCT_BRAND := Android
PRODUCT_MODEL := Android 15 for WSA
PRODUCT_MANUFACTURER := WSA-Next
