# This contains common definitions used to define a host module
# to link GoogleTest with the EmuGL test programs.
#
# This is used instead of including external/gtest/Android.mk to
# be able to build both the 32-bit and 64-bit binaries while
# building a 32-bit only SDK (sdk-eng, sdk_x86-eng, sdk_mips-eng).


LOCAL_PATH := $(EMULATOR_GTEST_SOURCES_DIR)

common_SRC_FILES := \
    src/gtest-all.cc \
    src/gtest_main.cc

common_CFLAGS := -O0 -Wno-unused-variable

ifneq (windows,$(BUILD_TARGET_OS))
    common_LDLIBS += -lpthread
endif

$(call emugl-begin-static-library,libemugl_gtest)
LOCAL_SRC_FILES := $(common_SRC_FILES)
LOCAL_CFLAGS += $(common_CFLAGS)
LOCAL_CPP_EXTENSION := .cc
$(call emugl-export,C_INCLUDES,$(LOCAL_PATH)/include)
$(call emugl-export,LDLIBS,$(common_LDLIBS))
$(call emugl-end-module)

ifdef FIRST_INCLUDE
    $(call emugl-begin-host-static-library,libemugl_gtest_host)
    LOCAL_SRC_FILES := $(common_SRC_FILES)
    LOCAL_CFLAGS += $(common_CFLAGS)
    LOCAL_C_INCLUDES += $(LOCAL_PATH)/include
    LOCAL_CPP_EXTENSION := .cc
    $(call emugl-export,C_INCLUDES,$(LOCAL_PATH)/include)
    $(call emugl-export,LDLIBS,$(common_LDLIBS) -lpthread)
    $(call emugl-end-module)
endif