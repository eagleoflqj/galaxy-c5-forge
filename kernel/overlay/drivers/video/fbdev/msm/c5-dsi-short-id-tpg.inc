/* SPDX-License-Identifier: GPL-2.0-only */
/* Differential test: bypass memory DMA fetch for three exact short ID reads. */
static atomic_t c5_dsi_short_id_tpg_count = ATOMIC_INIT(0);

static bool c5_dsi_short_id_tpg(struct mdss_dsi_ctrl_pdata *ctrl,
		struct dcs_cmd_req *req)
{
	struct dsi_cmd_desc *cmd;
	unsigned char code;

	/* The shared guard pins C5 recovery, DSI0, 1.3.1, PHY1, singleDSI.
	 * Native TPG support starts at 1.3.0. Keep normal bus/IOMMU votes,
	 * completion/error/timeout handling and all non-ID requests unchanged.
	 */
	if (!c5_dsi_tx_guard(ctrl) || !req || !req->rbuf ||
		!(req->flags & CMD_REQ_RX) ||
		(req->flags & CMD_REQ_DMA_TPG) ||
		req->rlen != 1 || req->cmds_cnt != 1 || !req->cmds)
		return false;
	cmd = req->cmds;
	if (!cmd->payload || cmd->dchdr.dtype != DTYPE_DCS_READ ||
		cmd->dchdr.dlen != 1 || cmd->dchdr.vc != 0 ||
		cmd->dchdr.last != 1)
		return false;
	code = (unsigned char)cmd->payload[0];
	if (code < 0xda || code > 0xdc ||
		!atomic_add_unless(&c5_dsi_short_id_tpg_count, 1, 3))
		return false;
	pr_warn("C5DSITPG short-ID=%02x native FIFO route;normal DMA bus votes retained\n",
		code);
	return true;
}
