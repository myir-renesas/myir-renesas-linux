#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sys_soc.h>
#include <linux/mm.h>
#include <linux/delay.h>

#define MAP_IO_REG(idx, field)							\
	do {									\
		ret = of_address_to_resource(np, idx, &res);			\
		if (ret)							\
			goto finish;						\
		priv->field = ioremap(res.start, resource_size(&res));		\
		if (!priv->field)						\
			goto finish;						\
	} while (0)

#define UNMAP_IO_REG(field)							\
	do {									\
		if (priv->field)						\
			iounmap(priv->field);					\
	} while (0)

struct rzt2h_mitigation {
	void __iomem *mstop, *vspd, *dmac, *usbf, *ddr_mirror;
};

void rzt2h_writel(void __iomem *base, u32 val, u16 offset)
{
	writel(val, base + offset);
};

u32 rzt2h_readl(void __iomem *base, u16 offset)
{
	return readl(base + offset);
};

void rzt2h_usbf_read(struct rzt2h_mitigation *priv)
{
	rzt2h_writel(priv->usbf, 0x66008, 0x42C);
	rzt2h_writel(priv->usbf, 0x0, 0x430);
	rzt2h_writel(priv->usbf, 0xc4000000, 0x400);
	rzt2h_writel(priv->usbf, 0x40, 0x408);
	rzt2h_writel(priv->usbf, 0x8, 0x428);
	mdelay(10);
	rzt2h_writel(priv->usbf, 0x5, 0x428);
	mdelay(10);

	while(rzt2h_readl(priv->usbf, 0x424) & 0x1);

	rzt2h_writel(priv->usbf, 0xc4000040, 0x404);
	rzt2h_writel(priv->usbf, 0x40, 0x408);
	rzt2h_writel(priv->usbf, 0x8, 0x428);
	mdelay(10);
	rzt2h_writel(priv->usbf, 0x5, 0x428);

	mdelay(10);
	while(rzt2h_readl(priv->usbf, 0x424) & 0x1);
};

void rzt2h_vspd_read(struct rzt2h_mitigation *priv)
{
	rzt2h_writel(priv->vspd, 0x0, 0x0);
	rzt2h_writel(priv->vspd, 0x1, 0x28);
	rzt2h_writel(priv->vspd, 0x11003, 0x48);
	rzt2h_writel(priv->vspd, 0x120, 0x78);
	rzt2h_writel(priv->vspd, 0x4, 0x300);
	rzt2h_writel(priv->vspd, 0x4, 0x304);
	rzt2h_writel(priv->vspd, 0x18, 0x308);
	rzt2h_writel(priv->vspd, 0x0, 0x30C);
	rzt2h_writel(priv->vspd, 0x0, 0x310);
	rzt2h_writel(priv->vspd, 0x1800000, 0x334);
	rzt2h_writel(priv->vspd, 0x1000, 0x33C);

	rzt2h_writel(priv->vspd, 0x2, 0x1000);
	rzt2h_writel(priv->vspd, 0x18, 0x100C);
	rzt2h_writel(priv->vspd, 0x180, 0x101C);
	rzt2h_writel(priv->vspd, 0x38, 0x2000);
	rzt2h_writel(priv->vspd, 0x3F, 0x2004);
	rzt2h_writel(priv->vspd, 0x500, 0x2014);
	rzt2h_writel(priv->vspd, 0x1000003F, 0x2050);

	rzt2h_writel(priv->vspd, 0x01000003, 0x3B00);
	rzt2h_writel(priv->vspd, 0x5DC, 0x3B04);
	rzt2h_writel(priv->vspd, 0x86000000, 0x3B0C);

	rzt2h_writel(priv->vspd, 0x1, 0x0);

	while(rzt2h_readl(priv->vspd, 0x7C) & 0x20);
};

int rzt2h_dmac_read(struct rzt2h_mitigation *priv)
{
	u32 chcfg, val1, val2;
	int count = 0;

	val1 = *(volatile u32 *)(priv->ddr_mirror + 0x1000);

	rzt2h_writel(priv->dmac, 0xC4001000, 0x0); /* N0SA(Next0 Source Address Register) Setting */

	rzt2h_writel(priv->dmac, 0xC4002000, 0x4); /* N0DA(Next0 Destination Address Register) Setting */

	rzt2h_writel(priv->dmac, 0x10, 0x8); /* N0TB(Next0 Transaction Byte Register) Setting */
	chcfg = (1 << 22)	/* Block transfer mode */
		| (2    << 16)	/* Destination transfer size 32 bit */
		| (2    << 12)	/* Source transfer size 32 bit */
		| (2    <<  4)	/* Detect rize edge for INTDMA */
		| (0 <<  0);	/* DMAREQ[0] */
	rzt2h_writel(priv->dmac, chcfg, 0x2C);
	rzt2h_writel(priv->dmac, 0x8, 0x28); /* CHCTRL(Channel Control Register) Status Clear */
	mdelay(10);
	rzt2h_writel(priv->dmac, 0x5, 0x28); /* DMA EN & STG(Software trigger) */
	mdelay(10);
	while (count < 5) {
		rzt2h_readl(priv->dmac, 0x24);
		count++;
		mdelay(10);
	}
	val2 = *(volatile u32 *)(priv->ddr_mirror + 0x2000);

	if (val1 != val2)
		return -EIO;

	return 0;

};

static const struct of_device_id renesas_socs[] __initconst = {
#ifdef CONFIG_ARCH_R9A09G077
	{ .compatible = "renesas,r9a09g077", },
#elif CONFIG_ARCH_R9A09G087
	{ .compatible = "renesas,r9a09g087", },
#endif
};

static const struct of_device_id renesas_ids[] __initconst = {
	{ .compatible = "renesas,rzt2h-mitigation", },
	{ }
};

static int __init rzt2h_mitigation_init(void)
{
	struct rzt2h_mitigation *priv;
	const struct of_device_id *match;
	struct device_node *np;
	struct resource res;
	int ret = 0;

	match = of_match_node(renesas_socs, of_root);
	if (!match)
		return -ENODEV;

	np = of_find_matching_node_and_match(NULL, renesas_ids, &match);
	if (!np) {
		pr_err("Missing Mitigation Device Node\n");
		return -ENOENT;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	MAP_IO_REG(0, mstop);
	MAP_IO_REG(1, vspd);
	MAP_IO_REG(2, dmac);
	MAP_IO_REG(3, usbf);

	ret = of_address_to_resource(np, 4, &res);
	if (ret)
		goto finish;

	priv->ddr_mirror = phys_to_virt(res.start);
	if (!priv->ddr_mirror) {
		ret = -ENOMEM;
		goto finish;
	}

	rzt2h_usbf_read(priv);
	rzt2h_vspd_read(priv);
	ret = rzt2h_dmac_read(priv);
	if (ret) {
		pr_err("%s: DMA transfer failed\n", match->compatible);
		goto finish;
	}

	pr_info("%s: Successed Initialization\n", match->compatible);

finish:
	UNMAP_IO_REG(usbf);
	UNMAP_IO_REG(dmac);
	UNMAP_IO_REG(vspd);
	UNMAP_IO_REG(mstop);
	return ret;
};
early_initcall(rzt2h_mitigation_init);
