# WSA-Next © 2026 Marek Wesołowski - wesmar
# Board configuration for Android 15 on the WSA 2407 host.

TARGET_ARCH := x86_64
TARGET_ARCH_VARIANT := x86_64
TARGET_CPU_ABI := x86_64
TARGET_2ND_ARCH := x86
TARGET_2ND_ARCH_VARIANT := x86_64
TARGET_2ND_CPU_ABI := x86

include build/make/target/board/BoardConfigMainlineCommon.mk

# WSA boots its own kernel and initrd; there is no bootloader or recovery.
TARGET_NO_KERNEL := true
TARGET_NO_BOOTLOADER := true
TARGET_NO_RECOVERY := true

# One plain ext4 image per WSA disk: no A/B, no super partition, no AVB
# (WSA's vendor fstab mounts the disks directly and verifies nothing).
AB_OTA_UPDATER := false
BOARD_AVB_ENABLE := false
BOARD_USES_SYSTEM_OTHER_ODEX :=

BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := ext4
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := ext4

# Optional private WSA integration layer (SELinux additions for its services).
-include vendor/wsa/BoardConfigWsa.mk

# The Google Play variant copies Google's ARM64 translator (guest ARM64 ELF files,
# vendor/ndk_translation) into /system; they cannot be cc_prebuilt modules of this
# x86_64 build.
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

# Images are sized to their content. BOARD_EXT4_SHARE_DUP_BLOCKS (inherited from
# BoardConfigMainlineCommon) deduplicates blocks, which keeps the download small and
# makes the images effectively read-only: every change goes through the build.

# Platform-side SELinux additions for the WSA host (properties used by patched vold).
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/wsa/wsa_x86_64/sepolicy/system_ext/private
