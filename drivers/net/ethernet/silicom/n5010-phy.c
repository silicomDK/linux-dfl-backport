// SPDX-License-Identifier: GPL-2.0
/* Intel Max10 BMC Lightning Creek phy Driver
 *
 * Copyright (C) 2020 Silicom Denmark A/S. All rights reserved.
 */
#include <linux/bits.h>
#include <linux/bitfield.h>
#include <linux/device.h>
#include <linux/mfd/intel-m10-bmc.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/phy.h>
#include <linux/phy_fixed.h>
#include <linux/platform_device.h>

#include "n5010-phy.h"

#define N5010_PHY_CSR_0		0x40c
#define N5010_PHY_CSR_1		0x410

#define N5010_PHY_ABSENT_0	BIT(7)
#define N5010_PHY_ABSENT_1	BIT(23)

#define N5010_PHY_LED_0		GENMASK(5, 3)
#define N5010_PHY_LED_1		GENMASK(21, 19)

#define N5010_GP_INPUT_CSR	0x080
#define N5010_BOARD_TYPE	GENMASK(19, 16)

struct n5010_phy {
	struct intel_m10bmc *m10bmc;
};

struct n5010_port {
	u64 num;
	bool sfp_in;
	struct n5010_phy *priv;
	struct phy_device *phy;
	bool (*get_link)(struct net_device *netdev);
};

static struct fixed_phy_status n5010_phy_status = {
	.link = 0,
	.speed = 1000,
	.duplex = 1,
};

static ssize_t board_info_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct n5010_phy *priv = dev_get_drvdata(dev);
	unsigned int csr = 0, board_type;
	char *board_info = "N/A";

	m10bmc_sys_read(priv->m10bmc, N5010_GP_INPUT_CSR, &csr);
	board_type = FIELD_GET(N5010_BOARD_TYPE, csr);
	if (board_type == 3)
	    board_info = "N5013";
	if (board_type == 4)
	    board_info = "N5014";

	return sysfs_emit(buf, "%s\n", board_info);
}

static DEVICE_ATTR_RO(board_info);

static struct attribute *n5010bmc_phy_attrs[] = {
	&dev_attr_board_info.attr,
	NULL,
};

ATTRIBUTE_GROUPS(n5010bmc_phy);

static int n5010_phy_sfp_status(struct n5010_port *port)
{
	unsigned int offset, bit, val;
	int ret;

	switch (port->num) {
	case 0:
		offset = N5010_PHY_CSR_1;
		bit = N5010_PHY_ABSENT_0;
		break;
	case 1:
		offset = N5010_PHY_CSR_1;
		bit = N5010_PHY_ABSENT_1;
		break;
	case 2:
		offset = N5010_PHY_CSR_0;
		bit = N5010_PHY_ABSENT_0;
		break;
	case 3:
		offset = N5010_PHY_CSR_0;
		bit = N5010_PHY_ABSENT_1;
		break;
	default:
		return -EINVAL;
	}

	ret = m10bmc_sys_read(port->priv->m10bmc, offset, &val);
	if (ret)
		return ret;

	port->sfp_in = !(val & bit);

	return 0;
}

static int n5010_phy_set_led(struct n5010_port *port, bool link)
{
	unsigned int offset, mask, val;

	switch (port->num) {
	case 0:
		offset = N5010_PHY_CSR_1;
		mask = N5010_PHY_LED_0;
		break;
	case 1:
		offset = N5010_PHY_CSR_1;
		mask = N5010_PHY_LED_1;
		break;
	case 2:
		offset = N5010_PHY_CSR_0;
		mask = N5010_PHY_LED_0;
		break;
	case 3:
		offset = N5010_PHY_CSR_0;
		mask = N5010_PHY_LED_1;
		break;
	default:
		return -EINVAL;
	}

	val = link ? mask : 0;

	return m10bmc_sys_update_bits(port->priv->m10bmc, offset, mask, val);
}

static void n5010_phy_adjust_link(struct net_device *netdev)
{
	struct n5010_port *port;
	bool link;
	int err;

	if (!netdev) {
		printk("n5010_phy_adjust_link called with netdev==NULL, ignored");
		return;
	}
	port = netdev->phydev->priv;
	link = netdev->phydev->link;

	netdev_info(netdev, "link: %i\n", link);

	err = n5010_phy_set_led(port, link);
	if (err)
		netdev_info(netdev, "failed to set led: %i\n", err);
}

static int n5010_phy_update_link(struct net_device *netdev,
				 struct fixed_phy_status *status)
{
	struct n5010_port *port;
	bool sfp_in;

	if (!netdev) {
		printk("n5010_phy_update_link called with netdev==NULL, ignored");
		return 0;
	}

	port = netdev->phydev->priv;
	sfp_in = port->sfp_in;

	n5010_phy_sfp_status(port);
	status->link = port->get_link(netdev);

	if (sfp_in != port->sfp_in)
		netdev_info(netdev, "sfp: %s\n", port->sfp_in ? "in" : "out");

	return 0;
}

int n5010_phy_module_info(struct net_device *netdev)
{
	struct n5010_port *port = netdev->phydev->priv;

	return port->sfp_in ? -ENODATA : -ENODEV;
}
EXPORT_SYMBOL(n5010_phy_module_info);

int n5010_phy_attach(struct device *dev, struct net_device *netdev,
		     bool (*get_link)(struct net_device *), u64 port_num)
{
	struct n5010_phy *priv = dev_get_drvdata(dev);
	struct phy_device *phy;
	struct n5010_port *port;
	int ret;

	phy = fixed_phy_register(PHY_POLL, &n5010_phy_status, NULL);
	if (IS_ERR(phy))
		return PTR_ERR(phy);

	port = devm_kzalloc(&phy->mdio.dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->num = port_num;
	port->priv = priv;
	port->phy = phy;
	port->get_link = get_link;

	phy->priv = port;

	ret = phy_connect_direct(netdev, phy, &n5010_phy_adjust_link,
				 PHY_INTERFACE_MODE_NA);
	if (ret)
		goto err_deregister;

	fixed_phy_set_link_update(phy, n5010_phy_update_link);
	fixed_phy_change_carrier(netdev, false);
	n5010_phy_sfp_status(port);

	netdev_info(netdev, "sfp: %s\n", port->sfp_in ? "in" : "out");

	return 0;

err_deregister:
	fixed_phy_unregister(phy);

	return ret;
}
EXPORT_SYMBOL(n5010_phy_attach);

int n5010_phy_detach(struct net_device *netdev)
{
	struct phy_device *phy = netdev->phydev;

	fixed_phy_change_carrier(netdev, false);

	phy_disconnect(phy);
	fixed_phy_unregister(phy);
	phy_device_free(phy);

	return 0;
}
EXPORT_SYMBOL(n5010_phy_detach);

static int n5010_phy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct n5010_phy *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	dev_set_drvdata(dev, priv);
	priv->m10bmc = dev_get_drvdata(dev->parent);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 0) && RHEL_RELEASE_CODE < 0x803
	return device_add_groups(dev, n5010bmc_phy_groups);
#endif

	return 0;
}

static const struct platform_device_id n5010_phy_ids[] = {
	{
		.name = "n5010bmc-phy",
	},
	{ }
};

static struct platform_driver n5010_phy_driver = {
	.probe = n5010_phy_probe,
	.driver = {
		.name = "n5010bmc-phy",
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 4, 0) || RHEL_RELEASE_CODE >= 0x803
		.dev_groups = n5010bmc_phy_groups,
#endif
	},
	.id_table = n5010_phy_ids,
};
module_platform_driver(n5010_phy_driver);

MODULE_DEVICE_TABLE(platform, n5010_phy_ids);
MODULE_AUTHOR("Intel Corporation");
MODULE_DESCRIPTION("Intel MAX10 BMC phy driver for n5010");
MODULE_LICENSE("GPL v2");
