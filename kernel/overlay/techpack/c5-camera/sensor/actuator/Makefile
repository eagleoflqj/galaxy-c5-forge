ccflags-y += -Itechpack/c5-camera
ccflags-y += -Itechpack/c5-camera/sensor/io
ccflags-y += -Itechpack/c5-camera/sensor/cci
obj-$(CONFIG_MSMB_CAMERA) += msm_actuator.o

# Preserve the original C5 zero-step lens parking quirk in this object only.
CFLAGS_msm_actuator.o += -DCONFIG_MACH_C5LTE_CHN_OPEN=1
