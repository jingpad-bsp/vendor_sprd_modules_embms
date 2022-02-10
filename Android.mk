ifdef_any_of = $(filter-out undefined,$(foreach v,$(1),$(origin $(v))))

ifneq ($(call ifdef_any_of, EMBMS_ENABLE MDT_ENABLE),)

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

LOCAL_SRC_FILES:= \
    embms.cpp

LOCAL_SHARED_LIBRARIES := \
    liblog \
    libutils \
    libcutils \
    libhardware_legacy \
    libhidlbase  \
    libhidltransport \
    libhwbinder \
    vendor.sprd.hardware.radio.lite@1.0 \

LOCAL_MODULE := embmsd
LOCAL_INIT_RC := embms.rc
LOCAL_MODULE_TAGS := optional

include $(BUILD_EXECUTABLE)

include $(call all-makefiles-under,$(LOCAL_PATH))
endif