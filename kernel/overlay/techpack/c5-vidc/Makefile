ifeq ($(CONFIG_C5_VIDC),y)
subdir-ccflags-y += -include $(srctree)/techpack/c5-vidc/c5-vidc-api.h
LINUXINCLUDE := -I$(srctree)/techpack/c5-vidc/include -I$(srctree)/techpack/c5-vidc/include/uapi $(LINUXINCLUDE)
obj-$(CONFIG_C5_VIDC) := 	msm_v4l2_vidc.o \
				msm_vidc_common.o \
				msm_vidc.o \
				msm_vdec.o \
				msm_venc.o \
				msm_smem.o \
				msm_vidc_debug.o \
				msm_vidc_res_parse.o \
				venus_hfi.o \
				hfi_response_handler.o \
				hfi_packetization.o \
				vidc_hfi.o \
				c5-vidc-native.o \
				c5-vidc-lifecycle.o \
				msm_vidc_dcvs.o \

obj-$(CONFIG_MSM_VIDC_VMEM) += vmem/

endif
