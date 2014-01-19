#Common headers
common_includes := hardware/qcom/display-caf/libgralloc
common_includes += hardware/qcom/display-caf/liboverlay
common_includes += hardware/qcom/display-caf/libcopybit
common_includes += hardware/qcom/display-caf/libqdutils
common_includes += hardware/qcom/display-caf/libhwcomposer
common_includes += hardware/qcom/display-caf/libexternal
common_includes += hardware/qcom/display-caf/libqservice
common_includes += hardware/qcom/display-caf/libvirtual

ifeq ($(TARGET_USES_POST_PROCESSING),true)
    common_flags     += -DUSES_POST_PROCESSING
    common_includes  += $(TARGET_OUT_HEADERS)/pp/inc
endif

common_header_export_path := qcom/display

#Common libraries external to display-caf HAL
common_libs := liblog libutils libcutils libhardware

#Common C flags
common_flags := -DDEBUG_CALC_FPS -Wno-missing-field-initializers
common_flags += -Werror

ifeq ($(ARCH_ARM_HAVE_NEON),true)
    common_flags += -D__ARM_HAVE_NEON
endif

ifneq ($(filter msm8974 msm8x74 msm8226 msm8x26,$(TARGET_BOARD_PLATFORM)),)
    common_flags += -DVENUS_COLOR_FORMAT
    common_flags += -DMDSS_TARGET
endif

# Executed only on QCOM BSPs
ifeq ($(TARGET_USES_QCOM_BSP),true)
# This flag is used to compile out any features that depend on framework changes
    common_flags += -DQCOM_BSP
endif

ifeq ($(call is-vendor-board-platform,QCOM),true)
common_deps += $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr
kernel_includes += $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ/usr/include
endif

ifeq ($(TARGET_DISPLAY_USE_RETIRE_FENCE),true)
    common_flags += -DUSE_RETIRE_FENCE
endif

ifneq ($(TARGET_DISPLAY_INSECURE_MM_HEAP),true)
    common_flags += -DSECURE_MM_HEAP
endif

ifneq ($(filter msm8660 msm7x30 msm7x27a,$(TARGET_BOARD_PLATFORM)),)
    common_flags += -DNO_IOMMU
endif
