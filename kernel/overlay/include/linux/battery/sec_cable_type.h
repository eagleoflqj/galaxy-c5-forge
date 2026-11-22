/* Samsung C5 cable-table ABI, derived from the GPL-2.0 stock kernel. */
#ifndef _SEC_CABLE_TYPE_H
#define _SEC_CABLE_TYPE_H
enum sec_cable_type {
	SEC_CABLE_UNKNOWN = 0,
	SEC_CABLE_BATTERY,			/* 1 */
	SEC_CABLE_UPS,				/* 2 */
	SEC_CABLE_MAINS,			/* 3 */
	SEC_CABLE_USB,				/* Standard Downstream Port (4) */
	SEC_CABLE_USB_DCP,			/* Dedicated Charging Port (5) */
	SEC_CABLE_USB_CDP,			/* Charging Downstream Port (6) */
	SEC_CABLE_USB_ACA,			/* Accessory Charger Adapters (7) */
	SEC_CABLE_BMS,				/* Battery Monitor System (8) */
	SEC_CABLE_MISC,				/* 9 */
	SEC_CABLE_WIRELESS,			/* 10 */
	SEC_CABLE_CARDOCK,			/* 11 */
	SEC_CABLE_UARTOFF,			/* 12 */
	SEC_CABLE_OTG,				/* 13 */
	SEC_CABLE_LAN_HUB,			/* 14 */
	SEC_CABLE_MHL_500,			/* 15 */
	SEC_CABLE_MHL_900,			/* 16 */
	SEC_CABLE_MHL_1500,			/* 17 */
	SEC_CABLE_MHL_2000,			/* 18 */
	SEC_CABLE_MHL_USB,			/* 19 */
	SEC_CABLE_MHL_USB_100,		/* 20 */
	SEC_CABLE_SMART_OTG,		/* 21 */
	SEC_CABLE_SMART_NOTG,		/* 22 */
	SEC_CABLE_POWER_SHARING,	/* power sharing cable (23) */
	SEC_CABLE_HV_PREPARE_MAINS,	/* Prepare Adaptive Charger (24) */
	SEC_CABLE_HV_ERR,			/* Adaptive Charger Err (25) */
	SEC_CABLE_HV_UNKNOWN,		/* Adaptive Charger Unknown (26) */
	SEC_CABLE_HV_MAINS,			/* Adaptive Charger (27) */
	SEC_CABLE_MDOCK_TA,			/* MMdock charger (28) */
	SEC_CABLE_USB_PARALLEL,		/* USB Parallel Path (29) */
	SEC_CABLE_HV_WIRELESS,		/* 30 */
	SEC_CABLE_PMA_WIRELESS,		/* 31 */
	SEC_CABLE_USB_HVDCP,		/* 32 */
	SEC_CABLE_HMT_CONNECTED,	/* 33 */
	SEC_CABLE_HMT_CHARGE,		/* 34 */
	SEC_CABLE_WIRELESS_PACK,		/* wireless charging pack (35) */
	SEC_CABLE_WIRELESS_PACK_TA,		/* wireless charging pack with TA (36) */
	SEC_CABLE_WIRELESS_STAND,		/* wireless charging stand pad type (37) */
	SEC_CABLE_WIRELESS_HV_STAND,	/* fast wireless charging stand pad type (38) */
	SEC_CABLE_HV_MAINS_12V,				/* 38 */
	SEC_CABLE_PDIC,             		/* 39 */
	SEC_CABLE_HV_MAINS_CHG_LIMIT,		/* 40 */
	SEC_CABLE_HV_QC20,			/* 41 - ILIM 1.8A - CHG 3.0A */
	SEC_CABLE_HV_QC30,			/* 42 - ILIM 3.0A - CHG 3.5A (Step Charging 4.2A -> 3.0A) */
	SEC_CABLE_MAX,

	SEC_CABLE_HV_WIRELESS_ETX = 100,
};
#endif
