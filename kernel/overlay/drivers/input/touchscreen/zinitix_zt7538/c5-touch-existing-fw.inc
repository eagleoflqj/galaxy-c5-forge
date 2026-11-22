/* SPDX-License-Identifier: GPL-2.0-only */
/* Resident startup policy stays independent of compiled update capability. */
static bool c5_tsp_recovery_preserve_fw(void)
{
	return IS_ENABLED(CONFIG_C5_RECOVERY_ONLY);
}

static bool c5_tsp_legacy_nvm_blocked(void)
{
	return c5_tsp_recovery_preserve_fw() ||
		IS_ENABLED(CONFIG_C5_CONTROLLER_FW_UPDATE);
}

/* All legacy command/data/read-select aliases are denied, even if a manual
 * one-shot token is armed. Only explicit stack-permit I/O may use these ports.
 * 0xc002/0xc001 resident startup and volatile CALIBRATE(6) remain source-scoped.
 */
static bool c5_tsp_nvm_register(u16 reg)
{
	switch (reg) {
	case ZT7538_SAVE_STATUS_CMD:
	case ZT7538_SAVE_CALIBRATION_CMD:
	case ZT7538_INIT_FLASH:
	case ZT7538_WRITE_FLASH:
	case ZT7538_READ_FLASH:
	case 0x01d3:
	case 0x01de:
	case 0x01df:
	case 0xc003:
	case 0xc104:
		return true;
	default:
		return false;
	}
}

static bool c5_tsp_controller_unavailable(struct zt7538_ts_info *info)
{
#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_UPDATE)
	return info && (READ_ONCE(info->c5_fw_busy) || READ_ONCE(info->c5_fw_failed) ||
		READ_ONCE(info->c5_fw_removing));
#else
	return false;
#endif
}

static bool c5_tsp_io_blocked(struct i2c_client *client, u16 reg)
{
#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_UPDATE)
	struct zt7538_ts_info *info = i2c_get_clientdata(client);
	if (info && (READ_ONCE(info->c5_fw_failed) ||
		(READ_ONCE(info->c5_io_owner) && READ_ONCE(info->c5_io_owner) != current)))
		return true;
#endif
	return c5_tsp_legacy_nvm_blocked() && c5_tsp_nvm_register(reg);
}

/* Serialize complete legacy send/receive operations. The logical manual owner
 * is checked while this mutex is held; the mutex is never held over power/init.
 * Normal volatile owner calls remain distinct from persistent permit I/O.
 */
static bool c5_tsp_io_enter(struct i2c_client *client)
{
#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_UPDATE)
	struct zt7538_ts_info *info = i2c_get_clientdata(client);
	if (info) {
		mutex_lock(&info->c5_io_lock);
		return true;
	}
#endif
	return false;
}

static void c5_tsp_io_exit(struct i2c_client *client, bool held)
{
#if IS_ENABLED(CONFIG_C5_CONTROLLER_FW_UPDATE)
	struct zt7538_ts_info *info = i2c_get_clientdata(client);
	if (held)
		mutex_unlock(&info->c5_io_lock);
#endif
}
