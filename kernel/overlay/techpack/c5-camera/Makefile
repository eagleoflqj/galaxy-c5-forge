ifeq ($(CONFIG_C5_CAMERA),y)
LINUXINCLUDE := -I$(srctree)/techpack/c5-camera/include -I$(srctree)/techpack/c5-camera/include/uapi $(LINUXINCLUDE)
subdir-ccflags-y += -include $(srctree)/techpack/c5-camera/c5-camera-api.h
GCOV_PROFILE := y
ccflags-y += -Itechpack/c5-camera
ccflags-y += -Itechpack/c5-camera/sensor
ccflags-y += -Itechpack/c5-camera/codecs
ccflags-y += -Itechpack/c5-camera/isps
ccflags-y += -Itechpack/c5-camera/pproc
ccflags-y += -Itechpack/c5-camera/msm_vb2
ccflags-y += -Itechpack/c5-camera/camera
ccflags-y += -Itechpack/c5-camera/jpeg_10
ccflags-y += -Itechpack/c5-camera/fd
ccflags-y += -Itechpack/c5-camera/smmu

obj-$(CONFIG_MSMB_CAMERA) += msm.o
obj-$(CONFIG_MSMB_CAMERA) += smmu/
obj-$(CONFIG_MSMB_CAMERA) += camera/
obj-$(CONFIG_MSMB_CAMERA) += msm_vb2/
obj-$(CONFIG_MSMB_CAMERA) += sensor/
obj-$(CONFIG_MSMB_CAMERA) += pproc/
obj-$(CONFIG_MSMB_CAMERA) += isp/
obj-$(CONFIG_MSMB_CAMERA) += ispif/
obj-$(CONFIG_MSMB_JPEG) += jpeg_10/
obj-$(CONFIG_MSMB_CAMERA) += msm_buf_mgr/
obj-$(CONFIG_MSM_FD) += fd/
endif
