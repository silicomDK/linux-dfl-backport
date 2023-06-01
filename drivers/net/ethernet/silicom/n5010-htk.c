// SPDX-License-Identifier: GPL-2.0

/* Silicom(R) Hitek 100G Network Driver
 *
 * Copyright (C) 2021-2023 Silicom Denmark. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/dfl.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/regmap.h>
#include <linux/delay.h>

#define CAPABILITY_OFFSET 0x08
#define CAP_AVAILABLE_RATES GENMASK_ULL(7, 0)
#define CAP_CONTAINS_PCS GENMASK_ULL(15, 8)
#define CAP_CONTAINS_FEC GENMASK_ULL(23, 16)
#define CAP_PORT_CNT GENMASK_ULL(43, 40)
#define CAP_RATE_1G BIT_ULL(0)
#define CAP_RATE_10G BIT_ULL(1)
#define CAP_RATE_25G BIT_ULL(2)
#define CAP_RATE_40G BIT_ULL(3)
#define CAP_RATE_50G BIT_ULL(4)
#define CAP_RATE_100G BIT_ULL(5)
#define CAP_RATE_200G BIT_ULL(6)
#define CAP_RATE_400G BIT_ULL(7)

#define MB_OFFSET 0x28
#define MB_PORT_SIZE 0x10

#define MAC_MAX_MTU 9022

#define MAC_OFFSET 0x000
#define STAT_OFFSET 0x100
#define PCS_OFFSET 0x800
#define XCVR_OFFSET 0x900

#define MAC_SET_TX_RX_PROMISC_ENABLE 0x6000533

struct n5010_htk_ops_params {
	struct stat_info *stats;
	u32 num_stats;
	struct n5010_htk_priv_flags *priv_flags;
	u32 num_priv_flags;
};

struct n5010_htk_netdata {
	struct regmap *regmap_mac;
	u32 priv_flags;
	const struct n5010_htk_ops_params *ops_params;
};

struct n5010_htk_drvdata {
	struct dfl_device *dfl_dev;
	void __iomem *base;
	u64 port_cnt;
	struct net_device *netdev[];
};

static netdev_tx_t netdev_xmit(struct sk_buff *skb, struct net_device *netdev)
{
	kfree_skb(skb);

	return NETDEV_TX_OK;
}

static const struct net_device_ops netdev_ops = {
	.ndo_start_xmit = netdev_xmit,
};

struct stat_info {
	unsigned int addr;
	char string[ETH_GSTRING_LEN];
};

#define STAT_INFO(_addr, _string) \
	.addr = _addr + STAT_OFFSET, .string = _string,

static void ethtool_get_stat_strings(struct net_device *netdev, u8 *data)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	unsigned int i, stats_num = 0;
	struct stat_info *stat;
	u8 *s = data;

	stat = npriv->ops_params->stats;
	stats_num = npriv->ops_params->num_stats;

	for (i = 0; i < stats_num; i++, s += ETH_GSTRING_LEN)
		strcpy(s, stat[i].string);
}

typedef void (*n5010_htk_feature_handler)(struct net_device *netdev, bool enable);

/* The flag bit is identical to index.
 */
struct n5010_htk_priv_flags {
	char flag_string[ETH_GSTRING_LEN];
	n5010_htk_feature_handler handler;
};

#define N5010_HTK_PRIV_FLAG(_name, _handler) {	\
	.flag_string = _name, \
	.handler = _handler, \
}

static void ethtool_get_priv_flag_strings(struct net_device *netdev, u8 *data)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	unsigned int i, priv_flags_num = 0;
	struct n5010_htk_priv_flags *priv_flags;
	u8 *s = data;

	priv_flags = npriv->ops_params->priv_flags;
	priv_flags_num = npriv->ops_params->num_priv_flags;

	for (i = 0; i < priv_flags_num; i++, s += ETH_GSTRING_LEN) {
		strcpy(s, priv_flags[i].flag_string);
	}
}

static void ethtool_get_strings(struct net_device *netdev, u32 stringset, u8 *data)
{
	switch (stringset) {
	case ETH_SS_STATS:
	    ethtool_get_stat_strings(netdev, data);
		break;
	case ETH_SS_PRIV_FLAGS:
	    ethtool_get_priv_flag_strings(netdev, data);
		break;
	default:
		break;
	}
}

static int ethtool_get_sset_count(struct net_device *netdev, int stringset)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);

	switch (stringset) {
	case ETH_SS_STATS:
		return npriv->ops_params->num_stats;
	case ETH_SS_PRIV_FLAGS:
		return npriv->ops_params->num_priv_flags;
	default:
		return -EOPNOTSUPP;
	}
}

static u64 read_mac_stat(struct regmap *regmap, unsigned int addr)
{
	u32 data_l;

	regmap_read(regmap, addr, &data_l);

	return data_l;
}

static void ethtool_get_stats(struct net_device *netdev,
			      struct ethtool_stats *stats, u64 *data)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	unsigned int i, stats_num = npriv->ops_params->num_stats;
	struct stat_info *stat = npriv->ops_params->stats;

	for (i = 0; i < stats_num; i++)
		data[i] = read_mac_stat(npriv->regmap_mac, stat[i].addr);
}

u32 ethtool_get_link(struct net_device *netdev)
{
	struct n5010_htk_netdata *npriv;
	u32 data_l;

	if (!netdev) {
		printk("n5010_htk_get_link called with netdev==NULL, ignored");
		return 0;
	}
	npriv = netdev_priv(netdev);

	// Read bit 0 in Deskew status register to determine link status
	regmap_read(npriv->regmap_mac, PCS_OFFSET + 0x10, &data_l);

	return data_l & 0x1;
}

static void n5010_htk_clear_stats(struct n5010_htk_netdata *npriv)
{
	unsigned int i, stats_num = npriv->ops_params->num_stats;
	struct stat_info *stat = npriv->ops_params->stats;

	for (i = 0; i < stats_num; i++)
		read_mac_stat(npriv->regmap_mac, stat[i].addr);
}

static struct stat_info stats_100g[] = {
	/* tx statistics */
	{ STAT_INFO(0x04, "tx_good") },
	{ STAT_INFO(0x0c, "tx_pause") },
	{ STAT_INFO(0x30, "tx_outerr") },
	{ STAT_INFO(0x60, "tx_ucast") },
	{ STAT_INFO(0x68, "tx_mcast") },
	{ STAT_INFO(0x70, "tx_bcast") },
	{ STAT_INFO(0x7c, "tx_single_coll") },
	{ STAT_INFO(0x80, "tx_multi_coll") },
	{ STAT_INFO(0x84, "tx_late_coll") },

	/* rx statistics */
	{ STAT_INFO(0x08, "rx_good") },
	{ STAT_INFO(0x10, "rx_pause") },
	{ STAT_INFO(0x14, "rx_crcerr") },
	{ STAT_INFO(0x18, "rx_ifgerr") },
	{ STAT_INFO(0x1c, "rx_alignerr") },
	{ STAT_INFO(0x20, "rx_oversize") },
	{ STAT_INFO(0x24, "rx_undersize") },
	{ STAT_INFO(0x28, "rx_truncated") },
	{ STAT_INFO(0x2c, "rx_inerr") },
	{ STAT_INFO(0x34, "rx_jabbers") },
	{ STAT_INFO(0x38, "rx_vlan") },
	{ STAT_INFO(0x3c, "rx_fragment") },
	{ STAT_INFO(0x40, "rx_all") },
	{ STAT_INFO(0x44, "rx_64b") },
	{ STAT_INFO(0x48, "rx_65to127b") },
	{ STAT_INFO(0x4c, "rx_128to255b") },
	{ STAT_INFO(0x50, "rx_256to511b") },
	{ STAT_INFO(0x54, "rx_512to1023b") },
	{ STAT_INFO(0x58, "rx_1024to1518b") },
	{ STAT_INFO(0x5c, "rx_1519tomaxb") },
	{ STAT_INFO(0x64, "rx_ucast") },
	{ STAT_INFO(0x6c, "rx_mcast") },
	{ STAT_INFO(0x74, "rx_bcast") },
};

static void n5010_htk_loopback(struct net_device *netdev, bool enable)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;
	u32 value = 0;

	netdev_info(netdev, "loopback %u", enable);
	// loopback enable is bit 12
	if (enable)
	    value = MAC_SET_TX_RX_PROMISC_ENABLE | BIT(12);
	else
	    value = MAC_SET_TX_RX_PROMISC_ENABLE;

	regmap_write(regmap, 0x4, value);
}

/* New flags can be added by just adding the a record with name and handler
 */
static struct n5010_htk_priv_flags gstrings_priv_flags[] = {
    N5010_HTK_PRIV_FLAG("loopback", n5010_htk_loopback),
};

#define PRIV_FLAGS_STR_LEN ARRAY_SIZE(gstrings_priv_flags)

static const struct n5010_htk_ops_params n5010_100g_params = {
	.stats = stats_100g,
	.num_stats = ARRAY_SIZE(stats_100g),
	.priv_flags = gstrings_priv_flags,
	.num_priv_flags = PRIV_FLAGS_STR_LEN,
};

static u32 n5010_htk_get_priv_flags(struct net_device *netdev)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	return	npriv->priv_flags;
}

static int n5010_htk_set_priv_flags(struct net_device *netdev, u32 flags)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	u32 changed_flags = npriv->priv_flags ^ flags;
	u32 i = 0;

	for (i = 0; i < PRIV_FLAGS_STR_LEN; i++) {
		const struct  n5010_htk_priv_flags *priv_flags = &npriv->ops_params->priv_flags[i];

		if (changed_flags & BIT(i)) {
			bool enable = !!(flags &  BIT(i));
			priv_flags->handler(netdev, enable);
		}
	}
	npriv->priv_flags = flags;
	return 0;
}

static const struct ethtool_ops ethtool_ops = {
	.get_strings = ethtool_get_strings,
	.get_sset_count = ethtool_get_sset_count,
	.get_ethtool_stats = ethtool_get_stats,
	.get_link = ethtool_get_link,
	.get_priv_flags	= n5010_htk_get_priv_flags,
	.set_priv_flags	= n5010_htk_set_priv_flags,
};

static void n5010_htk_init_netdev(struct net_device *netdev)
{
	netdev->ethtool_ops = &ethtool_ops;
	netdev->netdev_ops = &netdev_ops;
	netdev->features = 0;
	netdev->hard_header_len = 0;
	netdev->priv_flags |= IFF_NO_QUEUE;
	netdev->max_mtu = MAC_MAX_MTU;
	netdev->needs_free_netdev = true;

	ether_setup(netdev);
}

#ifndef devm_regmap_init_indirect_register
struct regmap *devm_regmap_init_indirect_register(struct device *dev,
						  void __iomem *base,
						  struct regmap_config *cfg);
#endif

#define REGMAP_NAME_SIZE 20
static struct regmap *n5010_htk_create_regmap(struct n5010_htk_drvdata *priv,
					      u64 port)
{
	void __iomem *base = priv->base + port * MB_PORT_SIZE;
	struct device *dev = &priv->dfl_dev->dev;
	struct regmap_config cfg = { 0 };
	char regmap_name[REGMAP_NAME_SIZE];

	scnprintf(regmap_name, REGMAP_NAME_SIZE, "n5010_htk%llu", port);
	base += MB_OFFSET;
	cfg.val_bits = 32;
	cfg.max_register = 0x1000;
	cfg.name = regmap_name;
	cfg.reg_bits = 32;

	return devm_regmap_init_indirect_register(dev, base, &cfg);
}

#define XCVR_CSR1_OFFSET XCVR_OFFSET + 0x4
#define DRP_OFFSET XCVR_OFFSET + 0x10
#define DRP_ADDR_OFFSET XCVR_OFFSET + 0x14
#define DRP_WDATA_OFFSET XCVR_OFFSET + 0x18
#define DRP_RDATA_OFFSET XCVR_OFFSET + 0x1C

/*
 * "Wait for the DRP_RDY register in the DRP Control/Status Register (offset 0x10) to be set again to
 * indicate that the DRP write operation is complete. Check additional DRP_STATUS register bits (if
 * available) in DRP Control/Status Register (offset 0x10) to ensure that no error has occurred."
 * No DRP_STATUS available for this design as it targets E-tile.
 */
#define WAIT_DRP_RDY(timeout_message) \
timeout = 0; \
drp_ready_status = 0; \
for (;;) { \
	regmap_read(regmap, DRP_OFFSET, &readdata); \
	drp_ready_status = (readdata >> 4) & 0x1; /* Indicates that the previously requested DRP read/write */ \
	/* operation is complete and a new DRP read/write operation can be initiated. */ \
	if (drp_ready_status != 0) { \
		break; \
	} else { \
		if (timeout > 0) { \
			printk(timeout_message, readdata, timeout, xcvr_channel); \
			return 0; \
		} \
		timeout++; \
	} \
}

/*
 * n5010_htk_drp_wr implements the DRP write Operation described in Hitek Systems
 * "Transceiver Wrapper User Guide" from May 10, 2021.
 * Section: "4.1 DRP Write Operation" (Comments in "" refers to the steps
 * described in that section of the document.)
 */
static u32 n5010_htk_drp_wr(struct regmap *regmap, u32 drp_addr, u32 drp_data,
			    u32 xcvr_channel)
{
	u32 readdata;
	u32 drp_ready_status = 0;
	u32 timeout = 0;

	// "Set the DRP_ENABLE register in the DRP Control/Status Register (offset 0x10)"
	// set DRP_ENABLE bit:
	regmap_write(regmap, DRP_OFFSET, 0x4);
	regmap_read(regmap, DRP_OFFSET, &readdata);

	WAIT_DRP_RDY("n5010_htk: ERROR - Timeout. n5010_htk_drp_wr Prior to write operation. DRP_RDY is stuck low. readdata = 0x%x, timeout=%d, xcvr=%d\n")

	// "Set the DRP_ADDR and DRP_WDATA register bits in DRP Address Register (offset 0x14) and DRP
	//  Write Data Register (offset 0x18) to write to the required transceiver or PLL register."
	regmap_write(regmap, DRP_ADDR_OFFSET, drp_addr + (xcvr_channel << 19));
	regmap_write(regmap, DRP_WDATA_OFFSET, drp_data);

	// "Set additional DRP_BLK_TYPE and DRP_BLK_NUM register bits (if available) in DRP Control/Status
	//  Register (offset 0x10) to select between multiple DRP blocks if a combination of multiple
	//  transceivers and PLLs."
	// Not applicable for this design as it targets an E-tile.

	// "Set the DRP_REQ register to ‘1’ and DRP_WR_RDn register to ‘1’ in the DRP Control/Status Register
	//  (offset 0x10) to initiate a DRP write operation."
	regmap_write(regmap, DRP_OFFSET, 0x7);

	WAIT_DRP_RDY("n5010_htk: ERROR - Timeout. n5010_htk_drp_wr After write operation. DRP_RDY is stuck low. readdata = 0x%x, timeout=%d, xcvr=%d\n")

	// return DRP status
	return drp_ready_status; // Success equals return: 1. (0 means timeout)
}

/*
 * n5010_htk_drp_wr implements the DRP read Operation described in Hitek Systems
 * "Transceiver Wrapper User Guide" from May 10, 2021.
 * Section: "4.2 DRP Read Operation" (Comments in "" refers to the steps
 * described in that section of the document.)
 */
static u32 n5010_htk_drp_rd(struct regmap *regmap, u32 drp_addr,
			    u32 xcvr_channel)
{
	u32 readdata;
	u32 drp_ready_status;
	u32 timeout;

	// "Set the DRP_ENABLE register in the DRP Control/Status Register (offset 0x10)"
	// set DRP_ENABLE bit
	regmap_write(regmap, DRP_OFFSET, 0x4);

	WAIT_DRP_RDY("n5010_htk: ERROR - Timeout. n5010_htk_drp_rd Prior to read operation DRP_RDY is stuck low. readdata = 0x%x, timeout=%d, xcvr=%d\n")

	// "Set the DRP_ADDR register bits in DRP Address Register (offset 0x14) to read from the required
	//  transceiver or PLL register."
	regmap_write(regmap, DRP_ADDR_OFFSET, drp_addr + (xcvr_channel << 19));

	// "Set additional DRP_BLK_TYPE and DRP_BLK_NUM register bits (if available) in DRP Control/Status
	//  Register (offset 0x10) to select between multiple DRP blocks if a combination of multiple
	//  transceivers and PLLs."
	// Not applicable for this design as it targets an E-tile.

	// "Set the DRP_REQ register to '1' and DRP_WR_RDn register to '0' in the DRP Control/Status Register
	//  (offset 0x10) to initiate a DRP read operation."
	regmap_write(regmap, DRP_OFFSET, 0x5);

	WAIT_DRP_RDY("n5010_htk: ERROR - Timeout. n5010_htk_drp_rd after read operation  DRP_RDY is stuck low. readdata = 0x%x, timeout=%d, xcvr=%d\n")

	// "Read the DRP_RDATA register bits from DRP Read Data Register (offset 0x1C)."
	regmap_read(regmap, DRP_RDATA_OFFSET, &readdata);
	return readdata;
}

/*
 * n5010_htk_drp_rmw wraps read, modify, write,
 * including xcvr_channel and a debug message before and after write with step number.
*/
static void n5010_htk_drp_rmw(struct net_device *netdev, u32 drp_addr, u32 modifier, u32 xcvr_channel, u32 step)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;
	u32 drp_rd = n5010_htk_drp_rd(regmap, drp_addr, xcvr_channel);
	u32 drp_ret;

	//netdev_dbg(netdev, "HTKDEBUG: rmw.%u. pre ch: %d read = 0x%x", step, xcvr_channel, drp_rd);
	drp_ret = n5010_htk_drp_wr(regmap, drp_addr, drp_rd | modifier, xcvr_channel);
	drp_rd  = n5010_htk_drp_rd(regmap, drp_addr, xcvr_channel);
	//netdev_dbg(netdev, "HTKDEBUG: rmw.%u. post ch: %d read = 0x%x write return = 0x%x", step, xcvr_channel, drp_rd, drp_ret);
}

/*
 * n5010_htk_pma_choose_config requests PMA configurationload load
 * and waits until ??. This is only performed on channel 0.
 */
static void n5010_htk_pma_choose_config(struct net_device *netdev, u32 step)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;

	u32 drp_rd;
	u32 timeout = 0;
	n5010_htk_drp_wr(regmap, 0x40143 /*Request PMA configuration load*/,
			0x80 /* Load PMA config 0l*/, 0 );
	do {
		drp_rd = n5010_htk_drp_rd(regmap, 0x40144 /*PMA Configuration Loading Status*/, 0);
		//netdev_dbg(netdev, "HTKDEBUG: %u. PMA configuration load status returns:: 0x%x", step, drp_rd);
		drp_rd = (drp_rd & 0x1);
		//netdev_dbg(netdev, "HTKDEBUG: %u. PMA configuration load status returns:: 0x%x", step, drp_rd);
		timeout++;
		if (timeout >= 1024) {
		   netdev_info(netdev, "HTKDEBUG: %u. ERROR. Timeout while waiting for  0x40144[0] = 1.", step);
		   break;
		}
	} while (drp_rd != 1);
}

/*
 * n5010_htk_load_pma_config_params loads params_code into registers
 * and waits until pma_done.
 */
static u32 n5010_htk_load_pma_config_params(struct net_device *netdev,
					     u32 xcvr_channel, u32 params_code)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;
	u32 ret;
	u32 pma_done = 0;
	u32 timeout = 0;
	ret = n5010_htk_drp_wr(regmap, 0x200 /*PMA register 0*/,
			       (params_code >> 0) & 0xFF /* PMA register data*/,
			       xcvr_channel);
	ret = n5010_htk_drp_wr(regmap, 0x201 /*PMA register 1*/,
			       (params_code >> 8) & 0xFF /* PMA register data*/,
			       xcvr_channel);
	ret = n5010_htk_drp_wr(regmap, 0x202 /*PMA register 2*/,
			       (params_code >> 16) &
				       0xFF /* PMA register data*/,
			       xcvr_channel);
	ret = n5010_htk_drp_wr(regmap, 0x203 /*PMA register 3*/,
			       (params_code >> 24) &
				       0xFF /* PMA register data*/,
			       xcvr_channel);

	do {
		u32 val_0x207 = n5010_htk_drp_rd(regmap, 0x207 /*PMA operation status*/,
				       xcvr_channel);
		pma_done = ((val_0x207 >> 7) & 0x1) & ((val_0x207 & 0x1) ^ 0x1); // Bit 7 = 1 => operation completed. Bit 0 = 0 => Operation completed successfully
		timeout++;
	} while ((pma_done != 1) && (timeout < 1024));
	if (timeout >= 1023)
	   netdev_info(netdev, "n5010_htk: ERROR timeout while Waiting for PMA operating mode setting status. readdata = 0x%x, timeout=%d, xcvr=%d\n",
			       ret, timeout, xcvr_channel);
	return pma_done; // Returns 1 on success. (0 means timeout)
}

/*
 * n5010_htk_update_pma_setting implements the attribute update flow described in Intels's
 * "E-tile Transceiver PHY User Guide" 683723. 2022-09-30. Section
 * "7.11. PMA Attribute Details"
 * Returns data or 0 if timeout.
 */
static int n5010_htk_update_pma_setting(struct net_device *netdev, u32 xcvr_channel, u32 pma_setting)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;
	u32 ret;
	u32 return_data;
	u32 pma_done = 0;
	u32 timeout = 0;

	ret = n5010_htk_drp_wr(regmap, 0x84 /*PMA code value [7:0] */,
			       (pma_setting >> 0) & 0xFF /* PMA register data*/,
			       xcvr_channel);
	ret = n5010_htk_drp_wr(regmap, 0x85 /*PMA code value [15:8] */,
			       (pma_setting >> 8) & 0xFF /* PMA register data*/,
			       xcvr_channel);
	ret = n5010_htk_drp_wr(regmap, 0x86 /*PMA code address [7:0] */,
			       (pma_setting >> 16) & 0xFF /* PMA register data*/,
			       xcvr_channel);
	ret = n5010_htk_drp_wr(regmap, 0x87 /*PMA code address [15:8] */,
			       (pma_setting >> 24) & 0xFF /* PMA register data*/,
			       xcvr_channel);

	// Step 2. Read modify write to set 0x90[0] to 1 PMA operation status:
	n5010_htk_drp_rmw(netdev, 0x90, 0x1, xcvr_channel, 2);

	// Step 3. Read 0x8A[7] until it is 1:
	timeout = 0;
	do {
		udelay(100);
		ret = n5010_htk_drp_rd(regmap, 0x8A /*PMA operation status*/,
				       xcvr_channel);
		pma_done = (ret >> 7) & 0x1;
		//printk("HTKDEBUG: n5010_htk_update_pma_setting Post write 0x8A readdata = 0x%x, xcvr=%d, When PMA attribute code is sent to the PMA this number is 1: 0x%x\n", ret, xcvr_channel, pma_done);
		timeout++;
	} while((pma_done != 1) && (timeout < 10000));
	if (timeout >= 10000) {
		netdev_info(netdev, "n5010_htk: ERROR n5010_htk_update_pma_setting Step 3. Timeout while waiting for PMA Attribute sent: 0x%x Post write 0x8A readdata = 0x%x, xcvr=%d, Timeout count: %d\n", pma_setting, ret, xcvr_channel, timeout);
		return 0;
	}

	// Step 4: Read 0x8B[0] until it is 0:
	timeout = 0;
	do {
		ret = n5010_htk_drp_rd(regmap, 0x8B /*[0] = 0 indicates that the PMA is done acting on the Attribute and code*/,
				       xcvr_channel);
		//printk("HTKDEBUG: n5010_htk_update_pma_setting Post write 0x8B readdata = 0x%x, xcvr=%d, PMA attribute code transaction completed when bit 0 of last vector is 0.\n", ret, xcvr_channel);
		timeout++;
		pma_done = 1 - (ret & 0x1); // pma_done will be 0 while bit [0] is one
	} while((pma_done != 1) && (timeout < 1024));
	if ( timeout >= 1024 ) {
		netdev_info(netdev, "ERROR n5010_htk_update_pma_setting Step 4. Timeout while waiting for PMA Attribute transaction complete. Post write 0x8B readdata = 0x%x, xcvr=%d, Timeout count: %d\n",
			ret, xcvr_channel, timeout);
		return 0;
	}
	//printk("HTKDEBUG: n5010_htk_update_pma_setting Step 4. Post write 0x8B readdata = 0x%x, xcvr=%d, Timeout count: %d. pma_done is: %d\n",
	//		       ret, xcvr_channel, timeout, pma_done);

	{
		u32 val_0x88;
		u32 val_0x89;
		// Step 5: Read the values of 0x89 and 0x88:
		val_0x89 = n5010_htk_drp_rd(regmap, 0x89 /* */, xcvr_channel);
		val_0x88 = n5010_htk_drp_rd(regmap, 0x88 /* */, xcvr_channel);
		return_data = ((val_0x89 << 8) | val_0x88);
		//printk("HTKDEBUG: DEBUG - n5010_htk_update_pma_setting Step 5. return_data value: 0x%x\n", return_data);
	}

	// Step 6: Write a 1 to 0x8A[7] to clear the 1 which should be there:
	n5010_htk_drp_rmw(netdev, 0x8A, 0x80, xcvr_channel, 6);
	return return_data;
}

static void n5010_htk_pma_verify_adaptation(struct net_device *netdev, u32 step)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;
	u32 xcvr_channel;

	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		u32 timeout = 0;
		u32 drp_ret;
		u32 drp_rd;
		for (;;) {
			drp_ret = n5010_htk_update_pma_setting(netdev, xcvr_channel, 0x01260B00);
			//netdev_dbg(netdev, "HTKDEBUG: %u. Starting initial adaptation for channel: %d,  PMA setting returns: 0x%x",
			//	       step, xcvr_channel, drp_ret);
			drp_rd = n5010_htk_drp_rd(regmap, 0x88 /*PMA attribute code return value [7:0]*/, xcvr_channel);
			drp_rd = (drp_rd & 0x1);
			//netdev_dbg(netdev, "HTKDEBUG: %u. Monitoring initial adaptation for channel: %d,  PMA setting returns: %d",
			//			step, xcvr_channel, drp_rd);
			if (drp_rd == 0) {
				break;
			} else {
				if (timeout >= 10000) {
					netdev_info(netdev, "HTKDEBUG: %u. ERROR Timeout while waiting for adaptation done for channel: %d, Register 0x88[0] returns: 0x%x, Timeout value: %d",
						step, xcvr_channel, drp_rd, timeout);
					break;
				}
				timeout++;
			}
		};
		netdev_dbg(netdev, "HTKDEBUG: %u. Monitoring of initial adaptation done for channel: %d, Register 0x88[0] returns: 0x%x, Timeout value: %d",
			step, xcvr_channel, drp_rd, timeout);
	}
}

/*
 * n5010_htk_reset asserts and de asserts reset to the transceivers.
 * and waits until reset done.
 * The reset done has been seen to be unstable so a 100 read done is used as target.
 * In rare cases it takes a long time before reset done is asserted, so a delay is
 * used and the timeout long. (Redoing reset, does not shorten the time).
 */
static u32 n5010_htk_reset(struct net_device *netdev)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;
	u32 tx_rx_ready_status_ok_cnt = 0;
	u32 tx_rx_ready_status = 0;
	u32 resample_cnt = 0;
	u32 timeout = 0;
	u32 readdata;

	regmap_read(
		regmap, XCVR_CSR1_OFFSET,
		&readdata); // after reconfiguration should contain 0xf00037f0

	regmap_write(regmap, XCVR_CSR1_OFFSET, (0xFFFFFFFD & readdata) | 0x2);
	netdev_dbg(netdev, "HTKDEBUG: n5010_htk_reset - The Reset has been asserted");

	do {
		udelay(1000);

	// Ensure that the transceivers are actually in reset:
		regmap_read(regmap, XCVR_CSR1_OFFSET, &readdata);
		tx_rx_ready_status = (readdata >> 12) & 0x3;
		if ( tx_rx_ready_status == 0x0 ) {
		   netdev_dbg(netdev, "HTKDEBUG: n5010_htk_reset asserted successfully. tx_rx_ready_status is = 0x%x\n", tx_rx_ready_status);
		} else {
		   netdev_dbg(netdev, "HTKDEBUG: ERROR: n5010_htk_reset NOT ASSERTED as expected. tx_rx_ready_status is = 0x%x\n", tx_rx_ready_status);
		}
		timeout++;
		if (timeout >= 200) {
			netdev_info(netdev, "ERROR n5010_htk_reset timeout, reset NOT ASSERTED, - Waited no: %d\n", timeout);
			return timeout;
		}
	} while (tx_rx_ready_status != 0x0);

	regmap_write(regmap, XCVR_CSR1_OFFSET, (0xFFFFFFFD & readdata));
	//netdev_dbg(netdev, "HTKDEBUG: n5010_htk_reset - The Reset has been DE-asserted");

	timeout = 0;
	while (tx_rx_ready_status_ok_cnt < 100) {
		regmap_read(regmap, XCVR_CSR1_OFFSET, &readdata);
		tx_rx_ready_status = (readdata >> 12) & 0x3;
		//netdev_dbg(netdev, "HTKDEBUG: n5010_htk_reset Attempt no: %d. tx_rx_ready_status is = 0x%x. Register value: 0x%x\n", timeout, tx_rx_ready_status, readdata);
		if ( tx_rx_ready_status == 0x3 ) {
			tx_rx_ready_status_ok_cnt++;
		} else {
			tx_rx_ready_status_ok_cnt = 0;
			resample_cnt++;
			udelay(200);
		}
		timeout++;
		if (timeout >= 20000) {
			netdev_info(netdev, "ERROR n5010_htk_reset timeout after - Attempt no: %d and samples: %d. tx_rx_ready_status is = 0x%x\n", timeout, resample_cnt, tx_rx_ready_status);
			return timeout;
		}
	}
	netdev_dbg(netdev, "HTKDEBUG: n5010_htk_reset successfully concluded after - Attempt no: %d and samples: %d. tx_rx_ready_status is = 0x%x\n", timeout, resample_cnt, tx_rx_ready_status);
	return 1; // success returns 1.
}

static u32 n5010_htk_wait_for_rx_freqlocked_1ms(struct net_device *netdev, u32 timeout_us)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;
	u32 timeout = 0;
	u32 chs_has_rx_traffic_cnt = 0; // Number of times in a row that all channels have Rx traffic
	u32 chs_w_rx_traffic;           // Number of channels with data arriving on the Rx
	u32 xcvr_channel;
	u32 drp_rd;
	u32 drp_rds_ok_cnt[4]={0,0,0,0};

	while (chs_has_rx_traffic_cnt <= 1000) {
		udelay(1);
		timeout++;
		chs_w_rx_traffic=0;
		for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
			drp_rd = n5010_htk_drp_rd(regmap, 0x40080 , xcvr_channel);
			//netdev_dbg(netdev, "HTKDEBUG: 11.5. DEBUG - Channel: %d Register 0x40080 with bit 0 being rx_locked_to_data: %x", xcvr_channel, drp_rd);
			chs_w_rx_traffic += (drp_rd & 0x1);
			// This eye height measurement does not work,
			//eye_heights[xcvr_channel] = n5010_htk_update_pma_setting(regmap, xcvr_channel, 0x002C1700); // 0x002C PMA Attribute code: Read. 0x1700 PMA Attribute data: Eye height.

			// DEBUG Code to update the ok counter:
			drp_rds_ok_cnt[xcvr_channel] += (drp_rd & 0x1);
		}
		//netdev_dbg(netdev, "HTKDEBUG: 11.5. DEBUG - Number of channels that have Rx traffic: %d", chs_w_rx_traffic);
		//netdev_dbg(netdev, "HTKDEBUG: 11.5. DEBUG - Eye heights. Ch3: %d, Ch2: %d,  Ch1: %d, Ch0: %d", eye_heights[3], eye_heights[2], eye_heights[1], eye_heights[0]);
		if (chs_w_rx_traffic == 4) {
			chs_has_rx_traffic_cnt++;
		} else {
			chs_has_rx_traffic_cnt = 0;
		}
		//netdev_dbg(netdev, "HTKDEBUG: 11.5. DEBUG - Number of times in a row that all channels have Rx traffic: %d", chs_has_rx_traffic_cnt);
		if  (timeout >= timeout_us) {
			netdev_info(netdev, "HTKDEBUG: ERROR Timeout while waiting for all channels to have Rx traffic. Timeout value in microseconds: %d", timeout);
			netdev_info(netdev, "HTKDEBUG: ERROR n5010_htk_wait_for_rx_freqlocked_1ms. rx_is_lockedtodata cnts: lane0: %d, lane1: %d, lane2: %d, lane3: %d", drp_rds_ok_cnt[0], drp_rds_ok_cnt[1], drp_rds_ok_cnt[2], drp_rds_ok_cnt[3]);
			return 0;
		}
	};
	netdev_dbg(netdev, "HTKexDEBUG: n5010_htk_wait_for_rx_freqlocked_1ms. rx_is_lockedtodata cnts: lane0: %d, lane1: %d, lane2: %d, lane3: %d", drp_rds_ok_cnt[0], drp_rds_ok_cnt[1], drp_rds_ok_cnt[2], drp_rds_ok_cnt[3]);
	netdev_dbg(netdev, "HTKexDEBUG: rx_freqlocked_1ms detected high - All channels have Rx traffic. Timeout value in microseconds: %d", timeout);
	return 1; // success returns 1
}

/*
 * n5010_htk_setup_regs activates Initial and Continuous adaptation for the transceivers.
 * It is done according to Intel's:
 * "E-Tile Transceiver PHY User Guide", 683723, 2023-03-30
 * Section "3.1.4. Duplex Adaptation Flow"
 * The HTKDEBUG numbers refer to the step numbers in the Duplex adaptation flow.
 * It has been changed slightly due to experiences gotten while designing the
 * the Adaptation Controller in RTL for the Front ports of N5014.
 */
static void n5010_htk_setup_regs(struct net_device *netdev)
{
	struct n5010_htk_netdata *npriv = netdev_priv(netdev);
	struct regmap *regmap = npriv->regmap_mac;
	u32 readdata;
	u32 drp_rd;
	u32 drp_ret;

	u32 xcvr_channel;
	//u32 eye_heights[4];
	u32 i;
	u32 success;
	u32 ia_cnt;
	u32 init_adapt_success_cnt = 0;

	netdev_dbg(netdev, "HTKDEBUG: 1. Start. assert and deassert Reset to the Native PHY by writing XCVR register offset 4 to 0x2");
	n5010_htk_reset(netdev);
	netdev_dbg(netdev, "HTKDEBUG: 1. End. Reset sequence.");

	netdev_dbg(netdev, "HTKDEBUG: 2. Start. Clear the register that indicates PMA attribute has been sent to the PMA.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		//Read-modify-write 0x8A[7] = 0x1 to clear PMA attributes sent.
		n5010_htk_drp_rmw(netdev, 0x8A, 0x80, xcvr_channel, 2);
	}
	netdev_dbg(netdev, "HTKDEBUG: 2. End.   Clear the register that indicates PMA attribute has been sent to the PMA.");

	netdev_dbg(netdev, "HTKDEBUG: 3. Start. Trigger PMA Analog Reset.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		netdev_dbg(netdev, "HTKDEBUG: 3. PMA Analog Reset xcvr_channel %d", xcvr_channel);
		n5010_htk_load_pma_config_params(netdev, xcvr_channel,	0x81000000 /* Write reset opcode to PMA UG-20056 6.4. PMA Analog Reset*/);
		drp_rd = n5010_htk_drp_rd(regmap, 0x8A /*PMA attribute status*/, xcvr_channel);
		netdev_dbg(netdev, "HTKDEBUG: 3. The very interesting register 0x8A. XCVR ch: %d has value: 0x%x", xcvr_channel, drp_rd);
	}
	netdev_dbg(netdev, "HTKDEBUG: 3. End.   Trigger PMA Analog Reset.");

	netdev_dbg(netdev, "HTKDEBUG: 4. Start. Wait 100 ms after reset.");

	for (i = 0; i < 10; i++) {
		udelay(10000);
	}

	netdev_dbg(netdev, "HTKDEBUG: 4. End. Wait 100 ms after reset.");

	netdev_dbg(netdev, "HTKDEBUG: 5. Start. Enable PMA calibration and re-load the initial PMA settings.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		netdev_dbg(netdev, "HTKDEBUG: 5. xcvr_channel: %d", xcvr_channel);

		//Read-modify-write 0x95[5] = 0x1 to enable PMA calibration when loading the new PMA settings.
		n5010_htk_drp_rmw(netdev, 0x95, 0x20, xcvr_channel, 4);

		//Read-modify-write 0x91[0] = 0x1 to load PMA settings into PMA.
		n5010_htk_drp_rmw(netdev, 0x91, 0x01, xcvr_channel, 4);

		drp_rd = n5010_htk_drp_rd(regmap, 0x8A /*PMA attribute status*/, xcvr_channel);
		netdev_dbg(netdev, "HTKDEBUG: 5. The very interesting register 0x8A. XCVR ch: %d has value: 0x%x", xcvr_channel, drp_rd);
	}
	netdev_dbg(netdev, "HTKDEBUG: 5. End.   Enable PMA calibration and re-load the initial PMA settings.");

	netdev_dbg(netdev, "HTKDEBUG: 6. Start. Set loopback and PRBS mode using Opcode.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		netdev_dbg(netdev, "HTKDEBUG: 6. Set loopback and PRBS mode for xcvr_channel: %d", xcvr_channel);
		n5010_htk_load_pma_config_params(
			netdev, xcvr_channel,
			0x9300000D /* Set Tx and Rx to PRBS31, Hard PRBS Gen, Enable loopback */);
	}
	netdev_dbg(netdev, "HTKDEBUG: 6. End.   Set loopback and PRBS mode using Opcode..");


	// This sets the Initial adaptation level to "Full effort" by reading from PMA
	// Attribute 0x0118 and then writing 0x0001 to it.
	// In RTL it has been attempted to do with full effort, but it took up to
	// a couple of minutes to bring a link up.
	// However: Low effort some times caused these Soft MAC links to break down
	// a short while after this driver was loaded. Besides these links come up in
	// a lot less than a minute even with Full effort.
	netdev_dbg(netdev, "HTKDEBUG: 7. Start. Set initial adaptation effort level.");

	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		drp_ret = n5010_htk_update_pma_setting(netdev, xcvr_channel, 0x002C0118); // 0x002C Attribute code: reads the Initial Adaptation effort level.
		netdev_dbg(netdev, "HTKDEBUG: 7. Setting initial adaptation level for channel: %x,  first PMA setting returns: 0x%x",
			       xcvr_channel, drp_ret);
		drp_ret = n5010_htk_update_pma_setting(netdev, xcvr_channel, 0x006C0001); // 0x006c attribute code Writes the Initial Adaptation effort level.
		netdev_dbg(netdev, "HTKDEBUG: 7. Setting initial adaptation level for channel: %x, second PMA setting returns: 0x%x",
			       xcvr_channel, drp_ret);
	}
	netdev_dbg(netdev, "HTKDEBUG: 7. End.   Set initial adaptation effort level.");

	netdev_dbg(netdev, "HTKDEBUG: 8. Start. Choose the PMA configuration."); // TBD: We need to setup 3 bits with which PMA configuration shall be used for initial adaptation.
	n5010_htk_pma_choose_config(netdev, 8);
	netdev_dbg(netdev, "HTKDEBUG: 8. End.   Choose the PMA configuration.");

	netdev_dbg(netdev, "HTKDEBUG: 9. Start. Load the configuration into the transceiver channel.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		netdev_dbg(netdev, "HTKDEBUG: 9. Load PMA configuration for xcvr_channel: %d",
		       xcvr_channel);
		n5010_htk_load_pma_config_params(
			netdev, xcvr_channel,
			0x94000000 /* Load PMA Configuration */);
	}
	netdev_dbg(netdev, "HTKDEBUG: 9. End.   Load the configuration into the transceiver channel.");

	netdev_dbg(netdev, "HTKDEBUG: 10. Start. Perform Initial adaptation.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		drp_ret = n5010_htk_update_pma_setting(netdev, xcvr_channel, 0x000A0001);
		netdev_dbg(netdev, "HTKDEBUG: 10. Starting initial adaptation for channel: %d,  PMA setting returns: 0x%x",
			       xcvr_channel, drp_ret);
	}
	netdev_dbg(netdev, "HTKDEBUG: 10. End.   Perform Initial adaptation.");

	udelay(10000); // Waiting 10 ms to ensure that the Initial Adaptation is actually started before waiting for it to complete

	netdev_dbg(netdev, "HTKDEBUG: 11. Start. Verify that the initial adaptation status is complete.");
	n5010_htk_pma_verify_adaptation(netdev, 11);
	success = n5010_htk_wait_for_rx_freqlocked_1ms(netdev, 2000000);
	if  (success == 0) {
		netdev_info(netdev, "HTKDEBUG: 11. ERROR Timeout while waiting for rx_locked for 1 millisecond.");
	}
	netdev_dbg(netdev, "HTKDEBUG: 11. End.   Verify that the initial adaptation status is complete.");

	netdev_dbg(netdev, "HTKDEBUG: 12. Start. Move to Mission/User Mode.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		n5010_htk_load_pma_config_params(netdev, xcvr_channel,
						 0x9300001E); // Disable; Internal Loopback and Hard PRBs generator
	}
	netdev_dbg(netdev, "HTKDEBUG: 12. End. Move to Mission/User Mode.");

	udelay(10000);

	netdev_dbg(netdev, "HTKDEBUG: 12.5. Start. Wait for Rx traffic available.");
	success = n5010_htk_wait_for_rx_freqlocked_1ms(netdev, 10000000);
	if  (success == 0) {
		netdev_info(netdev, "HTKDEBUG: 12.5. ERROR Timeout while waiting for all channels to have Rx traffic.");
	}

	netdev_dbg(netdev, "HTKDEBUG: 12.5. End.   Wait for Rx traffic available. Now it should be available.");

	netdev_dbg(netdev, "HTKDEBUG: 13. Set initial adaptation effort level. NOT IMPLEMENTED AS THE PREVIOUS SELECTED LEVEL IS REQUESTED");

	// Perform two succesfull initial adaptations in a row:
	for (ia_cnt = 1; ia_cnt <= 10; ia_cnt++) {
		netdev_dbg(netdev, "HTKDEBUG: 14. Start. Perform Initial adaptation for the %d. time", ia_cnt );
		for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
			drp_ret = n5010_htk_update_pma_setting(netdev, xcvr_channel, 0x000A0001);
			netdev_dbg(netdev, "HTKDEBUG: 14. Starting initial adaptation for channel: %d,  PMA setting returns: 0x%x",
				       xcvr_channel, drp_ret);
		}
		netdev_dbg(netdev, "HTKDEBUG: 14. End.   Perform Initial adaptation for the %d. time", ia_cnt);

		udelay(10000); // Waiting 10 ms to ensure that the Initial Adaptation is actually started before waiting for it to complete

		netdev_dbg(netdev, "HTKDEBUG: 15. Start. Verify that the initial adaptation status is complete for the %d. time", ia_cnt);
		n5010_htk_pma_verify_adaptation(netdev, 15);
		success = n5010_htk_wait_for_rx_freqlocked_1ms(netdev, 2000000);
		if (success == 1)
			init_adapt_success_cnt++;
		else
			init_adapt_success_cnt = 0;

		if  (init_adapt_success_cnt == 2) {
			netdev_dbg(netdev, "HTKDEBUG: 15. Initial adaptation has succesfully run two times in a row. Ready to proceed. Number of Initial Adaptation attempts: %d", ia_cnt);
			break;
		} else if (ia_cnt == 10) {
			netdev_info(netdev, "HTKDEBUG: 15. ERROR Initial Adaptation Failed! - attempts: %d", ia_cnt);
		}
		netdev_dbg(netdev, "HTKDEBUG: 15. End.   Verify that the initial adaptation status is complete for the %d. time", ia_cnt);
	}

	netdev_dbg(netdev, "HTKDEBUG: 16. Start. Choose the PMA configuration."); // We need to setup 3 bits with which PMA configuration shall be used for continuous adaptation.
	// These steps are only performed on channel 0:
	n5010_htk_pma_choose_config(netdev, 16);
	netdev_dbg(netdev, "HTKDEBUG: 16. End.   Choose the PMA configuration.");

	netdev_dbg(netdev, "HTKDEBUG: 17. Start. Load the configuration into the transceiver channel.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		netdev_dbg(netdev, "HTKDEBUG: 17. Load PMA configuration for xcvr_channel: %d",
		       xcvr_channel);
		n5010_htk_load_pma_config_params(
			netdev, xcvr_channel,
			0x94000001 /* Load PMA Configuration */);
	}
	netdev_dbg(netdev, "HTKDEBUG: 17. End.   Load the configuration into the transceiver channel.");

	netdev_dbg(netdev, "HTKDEBUG: 18. Start. Start Continuous Adaptation.");
	for (xcvr_channel = 0; xcvr_channel < 4; xcvr_channel++) {
		drp_ret = n5010_htk_update_pma_setting(netdev, xcvr_channel, 0x000A0006);
		netdev_dbg(netdev, "HTKDEBUG: 18. Starting Continuous adaptation for channel: %d,  PMA setting returns: 0x%x",
			       xcvr_channel, drp_ret);
	}
	netdev_dbg(netdev, "HTKDEBUG: 18. End.   Start Continuous Adaptation.");

	netdev_dbg(netdev, "HTKDEBUG: 19. Start. Reset Transceivers.");
	n5010_htk_reset(netdev);
	netdev_dbg(netdev, "HTKDEBUG: 19. End.   Reset Transceivers sequence.");

	//netdev_info(netdev, "HTKexDEBUG: 20. Check link status.");
	netdev_dbg(netdev, "HTKDEBUG: 20. Check link status.");
	success = n5010_htk_wait_for_rx_freqlocked_1ms(netdev, 2000000);
	if  (success == 0) {
		netdev_info(netdev, "HTKDEBUG: 20. ERROR Timeout while waiting for rx_locked for 1 millisecond. It should have been in Continuous Adaptation now.");
	}

	netdev_dbg(netdev, "HTKDEBUG: 21. Start transmitting and receiving data. So Continuous Adaptation is running");

	//recall iopll. Not documented in XCVR wrapper UG. Each bit in [27:24] being set is a request to recalibrate the IOPLLs.
	regmap_read(regmap, XCVR_CSR1_OFFSET, &readdata);
	regmap_write(regmap, XCVR_CSR1_OFFSET,
		     (readdata & 0xFEFFFFFF) | 0x1000000);

	regmap_read(regmap, XCVR_CSR1_OFFSET, &readdata);
	regmap_write(regmap, XCVR_CSR1_OFFSET,
		     (readdata & 0xFDFFFFFF) | 0x2000000);

	regmap_read(regmap, XCVR_CSR1_OFFSET, &readdata);
	regmap_write(regmap, XCVR_CSR1_OFFSET,
		     (readdata & 0xFBFFFFFF) | 0x4000000);

	regmap_read(regmap, XCVR_CSR1_OFFSET, &readdata);
	regmap_write(regmap, XCVR_CSR1_OFFSET,
		     (readdata & 0xF7FFFFFF) | 0x8000000);

	//netdev_dbg(netdev, "19. Enable RX PCS");
	regmap_read(regmap, PCS_OFFSET + 0x4, &readdata);
	//netdev_dbg(netdev, "HTKDEBUG: 19. Enable PCS RX in IP Core in FPGA Fabric");
	regmap_write(regmap, PCS_OFFSET + 0x4, 0x3); //enable rx+tx pcs
	regmap_read(regmap, PCS_OFFSET + 0x4, &readdata);
	//netdev_dbg(netdev, "HTKDEBUG: 19. Enable PCS RX in IP Core in FPGA Fabric");

	//20. not used

	for (i = 0; i < 5; i++) {
		netdev_dbg(netdev, "21. check RX PCS status");
		regmap_read(regmap, PCS_OFFSET + 0x8, &readdata);
		netdev_dbg(netdev, "PCS virtual lane alignment= 0X%x", readdata);
		regmap_read(regmap, PCS_OFFSET + 0xC, &readdata);
		netdev_dbg(netdev, "PCS AMLOCK= 0X%x", readdata);

		regmap_read(regmap, PCS_OFFSET + 0x10, &readdata);
		netdev_dbg(netdev, "PCS AM deskew= 0X%x", readdata);
	}

	// TX enable, enable tx mac
	regmap_write(regmap, 0x4, 0x6000521);
	// TX enable, enable rx mac + promisc
	regmap_write(regmap, 0x4, MAC_SET_TX_RX_PROMISC_ENABLE);
}

static int n5010_htk_create_netdev(struct n5010_htk_drvdata *priv, u64 port)
{
	struct device *dev = &priv->dfl_dev->dev;
	struct n5010_htk_netdata *npriv;
	struct net_device *netdev;
	int err = -ENOMEM;
	u32 timeout = 0;
	u32 link = 0;

	netdev = alloc_netdev(sizeof(struct n5010_htk_netdata), "n5010_htk%d",
			      NET_NAME_UNKNOWN, n5010_htk_init_netdev);

	if (!netdev)
		return -ENOMEM;

	npriv = netdev_priv(netdev);

	npriv->regmap_mac = n5010_htk_create_regmap(priv, port);
	if (!npriv->regmap_mac)
		goto err_free_netdev;

	npriv->ops_params = &n5010_100g_params;
	npriv->priv_flags = 0;

	SET_NETDEV_DEV(netdev, dev);

	priv->netdev[port] = netdev;

	/* Setup needed data to make netdev_dbg print without "(unnamed net_device) (uninitialized)"
	 * but without registering netdev to avoid seeing no link */
	scnprintf(netdev->name, IFNAMSIZ, "n5010_htk%llu", port);
	netdev->reg_state = NETREG_REGISTERED;

	n5010_htk_setup_regs(netdev);

	netdev->reg_state = NETREG_UNINITIALIZED;

	//dev_dbg(dev,"htk: 0x%x \n", regmap_read(npriv->regmap_mac, PCS_OFFSET+0x4));

	n5010_htk_clear_stats(npriv);

	// Wait until link with timeout of 2s
	for (;;) {
		link = ethtool_get_link(netdev);
		if (link == 1) {
			break;
		} else {
			if (timeout > 20000) {
				netdev_info(netdev, "DEBUG -  Link not detected within 2 seconds");
				break;
			}
		}
		timeout++;
		udelay(100);
	}

	err = register_netdev(netdev);
	if (err) {
		dev_err(dev, "failed to register %s: %d", netdev->name, err);
		goto err_unreg_netdev;
	}

	if (timeout > 20000) {
		netdev_info(netdev, "created, Link is down");
	} else {
		netdev_info(netdev, "created, Link is up");
	}
	netdev_dbg(netdev, "tests: %u", timeout);
	return 0;

err_unreg_netdev:
	unregister_netdev(netdev);
	return err;

err_free_netdev:
	free_netdev(netdev);
	return err;
}

static void n5010_htk_remove(struct dfl_device *dfl_dev)
{
	struct n5010_htk_drvdata *priv = dev_get_drvdata(&dfl_dev->dev);
	u64 port;

	for (port = 0; port < priv->port_cnt; port++) {
		struct net_device *my_netdev = priv->netdev[port];
		if (my_netdev)
			unregister_netdev(my_netdev);
	}
}

static int n5010_htk_probe(struct dfl_device *dfl_dev)
{
	struct device *dev = &dfl_dev->dev;
	struct n5010_htk_drvdata *priv;
	u64 val, port_cnt, port;
	void __iomem *base;
	u64 priv_size;
	int ret = 0;

	base = devm_ioremap_resource(dev, &dfl_dev->mmio_res);
	if (IS_ERR(base)) {
		ret = PTR_ERR(base);
		goto err_phy_dev;
	}

	val = readq(base + CAPABILITY_OFFSET);
	port_cnt = FIELD_GET(CAP_PORT_CNT, val);
	priv_size = sizeof(*priv) + port_cnt * sizeof(void *);

	priv = devm_kzalloc(dev, priv_size, GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_phy_dev;
	}

	dev_set_drvdata(dev, priv);

	priv->dfl_dev = dfl_dev;
	priv->port_cnt = port_cnt;
	priv->base = base;

	for (port = 0; port < priv->port_cnt; port++) {
		ret = n5010_htk_create_netdev(priv, port);
		if (ret) {
			n5010_htk_remove(dfl_dev);
			break;
		}
	}

err_phy_dev:
	return ret;
}

#define FME_FEATURE_ID_LL_100G_MAC_HTK_N5010				       \
	0x21 /* Silicom Lightning Creek Hitek */

static const struct dfl_device_id n5010_htk_mac_ids[] = {
	{ FME_ID, FME_FEATURE_ID_LL_100G_MAC_HTK_N5010 },
	{}
};

static struct dfl_driver n5010_htk_driver = {
	.drv = {
		.name = "n5010_htk",
	},
	.id_table = n5010_htk_mac_ids,
	.probe = n5010_htk_probe,
	.remove = n5010_htk_remove,
};

module_dfl_driver(n5010_htk_driver);
MODULE_ALIAS("dfl:t0000f0021");
MODULE_DEVICE_TABLE(dfl, n5010_htk_mac_ids);
MODULE_VERSION("0.0.1");
MODULE_DESCRIPTION("Network Device Driver for Silicom Lightning Creek Hitek");
MODULE_AUTHOR("Roger Christensen <rc@silicom.dk>");
MODULE_LICENSE("GPL v2");
