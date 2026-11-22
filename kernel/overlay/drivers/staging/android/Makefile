ccflags-y += -I$(src)			# needed for trace events

obj-y					+= ion/

obj-$(CONFIG_ASHMEM)			+= ashmem.o
obj-$(CONFIG_ANDROID_LOW_MEMORY_KILLER)	+= lowmemorykiller.o
obj-$(CONFIG_ANDROID_VSOC)		+= vsoc.o
obj-$(CONFIG_ANDROID_INTF_ALARM_DEV)	+= alarm-dev.o
obj-$(CONFIG_C5_LEGACY_SW_SYNC)		+= c5_legacy_sync.o c5_legacy_sw_sync.o

# Isolated generic compatibility component include path.
ccflags-y += -I$(srctree)/drivers/staging/android

obj-$(CONFIG_C5_LEGACY_ONESHOT_SYNC) += c5_legacy_oneshot_sync.o
