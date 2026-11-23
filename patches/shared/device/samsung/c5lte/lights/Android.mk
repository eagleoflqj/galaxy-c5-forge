# C5-specific legacy light HAL; keep the installed module name and both ABIs.
LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_SRC_FILES := lights.c
LOCAL_MODULE := lights.$(TARGET_BOARD_PLATFORM)
LOCAL_MODULE_RELATIVE_PATH := hw
LOCAL_MODULE_TAGS := optional
LOCAL_VENDOR_MODULE := true
LOCAL_MULTILIB := both
LOCAL_SHARED_LIBRARIES := liblog
LOCAL_CFLAGS := -Wall -Werror -DLOG_TAG=\"c5lights\"
include $(BUILD_SHARED_LIBRARY)
