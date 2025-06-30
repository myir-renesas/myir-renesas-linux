// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/pm_runtime.h>
#include <linux/delay.h>

#include "remoteproc_internal.h"

#define CR52_SRAM_START 	(0x10000000)
#define CR52_SRAM_END		(0x101FFFFF)
#define CR52_DDR_START		(0xE0000000)
#define CR52_DDR_END		(0xE1FFFFFF)
#define CA55_SRAM_START 	(0x00000000)
#define CA55_DDR_START		(0x180000000)
#define CA55_DDR_CR52_START (0x260000000)
#define CA55_DDR_CR52_END	(0x261FFFFFF)
#define CR52_TO_CA55_MASK	(0x0FFFFFFFF)

#define RSTSR0				(0x80280200)
#define SWRCPU0 			(0x81280220)
#define SWRCPU1 			(0x81280224)
#define MSTPCRN 			(0x81280334)
#define SSTPCR7 			(0x8129020C)
#define NS_SWINT			(0x802A0000)
#define PRCRN				(0x80294200)
#define PRCRS				(0x81296000)
#define CPU1HALT			(0x81295010)

#define BSP_PRV_RESET_KEY								(0x4321A501)
#define BSP_PRV_RESET_KEY_AUTO_RELEASE					(0x4321A502)
#define BSP_PRV_RESET_RELEASE_KEY						(0x00000000)
#define BSP_PRV_ATCM_AXIS_CR520_ADDRESS 				(0x20000000)
#define BSP_PRV_ATCM_AXIS_CR521_ADDRESS 				(0x21000000)
#define BSP_PRV_IMAGE_INFO_BRANCH_INSTRUCTION_CR520 	(0xE51FF004)
#define BSP_PRV_IMAGE_INFO_BRANCH_INSTRUCTION_CR521 	(0xE51FF000)
#define BSP_PRV_IMAGE_INFO_BRANCH_ADDRESS_CR520 		(0x10061000)
#define BSP_PRV_IMAGE_INFO_BRANCH_ADDRESS_CR521 		(0x10061000)
#define BSP_PRV_PRCR_KEY								(0x0000A500)
#define PRCR_WRITE_ENABLE_ALL_MASK						(0x0000000F)
#define CR520_MODULE_STOP_MASK							(0x00000001)
#define CR521_MODULE_STOP_MASK							(0x00000002)
#define CR520_RSTSR0_MASK								(0x00000010)
#define CR521_RSTSR0_MASK								(0x00000020)
#define SSTPCR7_AXIS1_ACK_MASK							(0x00000020)
#define CLEAR_RESET_STATUS_VALUE						(0x00000000)

#define RSC_TBL_SIZE			(0x1000)

#define NS_SWINT_MIN_CHANNEL		(0)
#define NS_SWINT_MAX_CHANNEL		(13)

struct rz_rproc_pdata {
	struct reset_control *reset2;
	struct reset_control *reset0;
	struct reset_control *reset1;
	struct regmap *cpg_regmap;
	struct regmap *sysc_regmap;
	u32 bootaddr[2];
	u32 core;
	u32 swint;
	u32 start_addr;
};

static int rz_rproc_mem_alloc(struct rproc *rproc,
				 struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void __iomem *va;
	dev_dbg(dev, "map memory: %pa+%zx\n", &mem->dma, mem->len);
	va = devm_ioremap_wc(dev, mem->dma, mem->len);

	if (!va) {
		dev_err(dev, "unable to map memory region: %pa+%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	/* Update memory entry va */
	mem->va = va;
	
	return 0;
}

static int rz_rproc_mem_release(struct rproc *rproc,
				   struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;

	dev_dbg(dev, "unmap memory: %pa\n", &mem->dma);
	devm_iounmap(dev, mem->va);
	
	return 0;
}

static int rz_rproc_prepare(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct platform_device *pdev = to_platform_device(dev);
	struct device_node *np = dev->of_node;
	struct of_phandle_iterator it;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;
	struct resource *res;
	int index = 0;
	int i;
	u64 da;

	/* Register resources */
	for (i = 0; i < pdev->num_resources; i++) {
		res = pdev->resource + i;

		/* No need to translate pa to da, RZ/G3S use same map */
		da = res->start;

		mem = rproc_mem_entry_init(dev, NULL,
					   res->start,
					   resource_size(res), da,
					   rz_rproc_mem_alloc,
					   rz_rproc_mem_release,
					   res->name);
		if (!mem)
			return -ENOMEM;

		rproc_add_carveout(rproc, mem);
	}

	/* Register associated reserved memory regions */
	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {
		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			dev_err(dev, "unable to acquire memory-region\n");
			return -EINVAL;
		}

		if (rmem->base > U64_MAX){
			return -EINVAL;
		}

		/* No need to translate pa to da, RZ/G3S use same map */
		da = rmem->base;

		/*  No need to map vdev buffer */
		if (strcmp(it.node->name, "vdev0buffer")) {
			mem = rproc_mem_entry_init(dev, NULL,
						   rmem->base,
						   rmem->size, da,
						   rz_rproc_mem_alloc,
						   rz_rproc_mem_release,
						   it.node->name);
		} else {
			mem = rproc_of_resm_mem_entry_init(dev, index,
							   rmem->size,
							   rmem->base,
							   it.node->name);
		}
		
		if (!mem)
			return -ENOMEM;

		rproc_add_carveout(rproc, mem);
		index++;
	}
	
	return 0;
}

static int rz_rproc_start(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct rz_rproc_pdata *pdata = rproc->priv;
	void __iomem *atcm_base_0;
	void __iomem *atcm_base_1;
	void __iomem *prcrs_base;
	void __iomem *prcrn_base;
	void __iomem *cpu1halt_base;
	void __iomem *sstpcr7_base;
	void __iomem *reg_base1;
	void __iomem *reg_base2;
	u32 reg_val;

	/* Ioremap used register*/
	prcrs_base = ioremap(PRCRS, 0x4);	
	if (!prcrs_base) {
		dev_err(&rproc->dev,"Failed to map memory\n");
		return -ENOMEM;
	}

	prcrn_base = ioremap(PRCRN, 0x4);	
	if (!prcrs_base) {
		dev_err(&rproc->dev,"Failed to map memory\n");
		return -ENOMEM;
	}

	atcm_base_0 = ioremap(BSP_PRV_ATCM_AXIS_CR520_ADDRESS, 0x4);
	if (!atcm_base_0) {
		dev_err(&rproc->dev, "Failed to map memory\n");
		return -ENOMEM;
	}

	atcm_base_1 = ioremap(BSP_PRV_ATCM_AXIS_CR521_ADDRESS, 0x8);
	if (!atcm_base_1) {
		dev_err(&rproc->dev, "Failed to map memory\n");
		return -ENOMEM;
	}

	cpu1halt_base = ioremap(CPU1HALT, 0x4);
	if (!cpu1halt_base) {
		dev_err(&rproc->dev, "Failed to map memory\n");
		return -ENOMEM;
	}

	sstpcr7_base = ioremap(SSTPCR7, 0x4);
	if (!sstpcr7_base) {
		dev_err(&rproc->dev, "Failed to map memory\n");
		return -ENOMEM;
	}

	reg_base1 = ioremap(0x81280000, 0x4);	
	if (!reg_base1) {
		dev_err(&rproc->dev, "Failed to map memory\n");
		return -ENOMEM;
	}

	reg_base2 = ioremap(0x80280000, 0x4);	
	if (!reg_base2) {
		dev_err(&rproc->dev, "Failed to map memory\n");
		return -ENOMEM;
	}

	/* Setting Area protect register (Non-Safety and Safety)
	** Enables writing to the registers related to the clock generation circuit. 
	** Enables writing to the registers related to low power consumption and reset. 
	** Enables writing to the registers related to GPIO settings. 
	** Enables writing to the registers related to System control.
	*/
	reg_val = ioread32(prcrn_base) | BSP_PRV_PRCR_KEY | PRCR_WRITE_ENABLE_ALL_MASK;
	iowrite32(reg_val, prcrn_base);
	reg_val = ioread32(prcrs_base) | BSP_PRV_PRCR_KEY | PRCR_WRITE_ENABLE_ALL_MASK;
	iowrite32(reg_val, prcrs_base);

	if(pdata->core == 0)
	{
		/* Store the instruction code to start address of the ATCM of CPU0 via AXIS interface*/
		iowrite32(BSP_PRV_IMAGE_INFO_BRANCH_INSTRUCTION_CR520, atcm_base_0);
		iowrite32(pdata->start_addr, atcm_base_0 + 0x4);

		/* Reset CR52_0 and release the reset state by setting 0x4321A502 to SWRCPU0 register */
		iowrite32(BSP_PRV_RESET_KEY_AUTO_RELEASE, reg_base1 + 0x220);			//SWRCPU0 (0x81280220)
	}
	else
	{
		/* Release from the module stop state by resetting MSTPCRN01 bit in MSTPCRN register */ 
		reg_val = ioread32(reg_base1 + 0x334) & (~CR521_MODULE_STOP_MASK);		
		iowrite32(reg_val, reg_base1 + 0x334);					//MSTPCRN (0x81280334)

		/* Release CR52_1 from the reset state by setting 0x00000000 to SWRCPU1 register. */
		iowrite32(BSP_PRV_RESET_RELEASE_KEY, reg_base1 + 0x224);				//SWRCPU1 (0x81280224)

		/* Release from the slave stop state by controlling AXIS1_REQ and AXIS1_ACK bits in SSTPCR7 register. */
		iowrite32(0x00000000, sstpcr7_base);	
		do
		{
			reg_val = ioread32(sstpcr7_base);
		} while (reg_val & SSTPCR7_AXIS1_ACK_MASK != 0);

		/* Store the instruction code to start address of the ATCM of CPU1 via AXIS interface */
		iowrite64(BSP_PRV_IMAGE_INFO_BRANCH_INSTRUCTION_CR521, atcm_base_1);
		iowrite64(pdata->start_addr, atcm_base_1 + 0x8);

		/* Start instruction fetch by writing 0 to CPU1HALT bit in CPU1HALT register */
		iowrite32(0x00000000, cpu1halt_base);
	}

	/* ioumap register */
	iounmap(prcrs_base);
	iounmap(prcrn_base);
	iounmap(atcm_base_0);
	iounmap(atcm_base_1);
	iounmap(cpu1halt_base);
	iounmap(sstpcr7_base);
	iounmap(reg_base1);
	iounmap(reg_base2);

	return 0;
}

static int rz_rproc_stop(struct rproc *rproc)
{
	struct rz_rproc_pdata *pdata = rproc->priv;
	struct rproc_mem_entry *carveout;
	void __iomem *prcrn_base;
	void __iomem *prcrs_base;
	void __iomem *swint_base;
	void __iomem *rstsr0_base;
	void __iomem *swrcpu1_base;
	u32 reg_val;
	

	/* Ioremap used register*/
	prcrn_base = ioremap(PRCRN, 0x4);	
	if (!prcrn_base) {
		dev_err(&rproc->dev,"Failed to map memory\n");
		return -ENOMEM;
	}

	prcrs_base = ioremap(PRCRS, 0x4);	
	if (!prcrs_base) {
		dev_err(&rproc->dev,"Failed to map memory\n");
		return -ENOMEM;
	}

	swint_base = ioremap(NS_SWINT, 0x4);	
	if (!swint_base) {
		dev_err(&rproc->dev,"Failed to map memory\n");
		return -ENOMEM;
	}

	rstsr0_base = ioremap(RSTSR0, 0x4);	
	if (!rstsr0_base) {
		dev_err(&rproc->dev, "Failed to map memory\n");
		return -ENOMEM;
	}

	swrcpu1_base = ioremap(SWRCPU1, 0x4);	
	if (!swrcpu1_base) {
		dev_err(&rproc->dev, "Failed to map memory\n");
		return -ENOMEM;
	}

	/* Setting Area protect register (Non-Safety and Safety)
	** Enables writing to the registers related to the clock generation circuit. 
	** Enables writing to the registers related to low power consumption and reset. 
	** Enables writing to the registers related to GPIO settings. 
	** Enables writing to the registers related to System control.
	*/
	reg_val = ioread32(prcrn_base) | BSP_PRV_PRCR_KEY | PRCR_WRITE_ENABLE_ALL_MASK;
	iowrite32(reg_val, prcrn_base);
	reg_val = ioread32(prcrs_base) | BSP_PRV_PRCR_KEY | PRCR_WRITE_ENABLE_ALL_MASK;
	iowrite32(reg_val, prcrs_base);

	/* Set software interrupt to change CR52 to WFI state */
	if((NS_SWINT_MAX_CHANNEL >= pdata->swint) && (pdata->swint >= NS_SWINT_MIN_CHANNEL))
	{
		reg_val = 0x01 << (pdata->swint);
		iowrite32(reg_val, swint_base);
	}

	/* Clear reset status flag */
	reg_val = ioread32(rstsr0_base);
	iowrite32(CLEAR_RESET_STATUS_VALUE, rstsr0_base);

	if(pdata->core == 1)
	{
		/* Transfer CR52_1 to reset state */
		iowrite32(BSP_PRV_RESET_KEY, swrcpu1_base);
		do
		{
			reg_val = ioread32(swrcpu1_base);
		} while (reg_val != 0x01);
	}

	/* ioumap register */
	iounmap(prcrs_base);
	iounmap(prcrn_base);
	iounmap(swint_base);
	iounmap(rstsr0_base);
	iounmap(swrcpu1_base);

	return 0;
}

static int rz_rproc_attach(struct rproc *rproc)
{
	/* Do nothing */
	return 0;
}

static void rz_rproc_kick(struct rproc *rproc, int vqid)
{
	/* Not supported Linux RPMsg yet */
}

static int cr52_to_ca55(u64 *da)
{
	printk("da=%lx\n",*da);
	if ((CR52_SRAM_END >= *da) && (*da >= CR52_SRAM_START)) {
		*da = CA55_SRAM_START + *da;
		printk("ca_sram=%lx da=%lx end-%lx\n",CA55_SRAM_START,*da,CR52_SRAM_END);
		return 0;
	}
	else if ((CR52_DDR_END >= *da) && (*da >= CR52_DDR_START)) {
		*da = CA55_DDR_START + *da;
		printk("CA55_DDR_START=%lx %lx\n",CA55_DDR_START,CA55_DDR_START + *da);
		printk("cr52_ddr_end1=%lx da=%lx CR52_DDR_START=%x\n",CR52_DDR_END,*da,CR52_DDR_START);
		return 0;
	}
	else
	{
		printk("cr52_ddr_end=%lx da=%lx CR52_DDR_START=%lx\n",CR52_DDR_END,*da,CR52_DDR_START);
		return -EINVAL;
	}
}

static void *rz_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len)
{
	struct device *dev = rproc->dev.parent;
	struct rproc_mem_entry *carveout;
	void *ptr = NULL;
	int ret;

	/* rproc_da_to_va() is called in many places. @da value can either be
	 * the address of segments in .elf file which is in CM33 address space
	 * or the address of .resource_table's trace buffer which is in CA55
	 * address space. Trace buffer is expected to be in the dedicated memory
	 * region for CM33 in DDR. Here, we first check if @da is address of
	 * trace buffer or segments then have corresponding action.
	 */
	if ((CA55_DDR_CR52_END >= da) && (da >= CA55_DDR_CR52_START)) {
		/* @da is address of trace buffer. Do nothing. */
	} else {
		/* @da is address of segment. Translate @da to CA55 space. */
		ret = cr52_to_ca55(&da);
		if (ret) {
			dev_err(dev, "invalid address\n");
			return ptr;
		}
	}

	list_for_each_entry(carveout, &rproc->carveouts, node) {
		int offset = da - carveout->da;

		/* Verify that carveout is allocated */
		if (!carveout->va)
			continue;

		/* try next carveout if da is too small */
		if (offset < 0)
			continue;

		/* try next carveout if da is too large */
		if (offset + len > carveout->len)
			continue;

		ptr = carveout->va + offset;
		break;
	}
	
	return ptr;
}

static int rz_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	int ret;

	ret = rproc_elf_load_rsc_table(rproc, fw);
	if (ret)
		dev_warn(&rproc->dev, "no resource table found for this firmware\n");

	return 0;
}

static const struct rproc_ops rz_rproc_ops = {
	.prepare		= rz_rproc_prepare,
	.start			= rz_rproc_start,
	.stop			= rz_rproc_stop,
	.attach			= rz_rproc_attach,
	.kick			= rz_rproc_kick,
	.da_to_va		= rz_rproc_da_to_va,
	.parse_fw		= rz_rproc_parse_fw,
	.find_loaded_rsc_table	= rproc_elf_find_loaded_rsc_table,
	.load			= rproc_elf_load_segments,
	.sanity_check		= rproc_elf_sanity_check,
	.get_boot_addr		= rproc_elf_get_boot_addr,
};

static int rz_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct rz_rproc_pdata *pdata;
	struct rproc *rproc;
	int ret;

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	rproc = devm_rproc_alloc(dev, np->name, &rz_rproc_ops, NULL,
				 sizeof(*pdata));
	if (!rproc)
		return -ENOMEM;

	/* Get remoteproc core, Non-safety sofware interrupt channel, FW start address */
	of_property_read_u32_index(np, "renesas,rz-core", 0, &pdata->core);
	of_property_read_u32_index(np, "renesas,rz-swint", 0, &pdata->swint);
	of_property_read_u32_index(np, "renesas,rz-start_address", 0, &pdata->start_addr);

	rproc->priv = pdata;
	rproc->auto_boot = of_get_property(np, "renesas,rz-autoboot", NULL) ?
			   true : false;

	pm_runtime_enable(dev);
	
	platform_set_drvdata(pdev, rproc);

	/* Register remote processor */
	ret = rproc_add(rproc);
	if (ret) {
		dev_err(dev, "failed to register rproc\n");
		goto error;
	}

	dev_info(dev, "probed\n");

	return 0;

error:
	rproc_free(rproc);

	pm_runtime_disable(dev);

	return ret;
}

static int rz_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);

	rproc_del(rproc);

	rproc_free(rproc);

	pm_runtime_disable(&pdev->dev);

	return 0;
}

static const struct of_device_id rz_rproc_of_match[] = {
	{ .compatible = "renesas,rz-cr52", },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, rz_rproc_of_match);

static struct platform_driver rz_rproc_driver = {
	.probe	= rz_rproc_probe,
	.remove = rz_rproc_remove,
	.driver = {
		.name = "rz-rproc",
		.of_match_table = rz_rproc_of_match,
	},
};
module_platform_driver(rz_rproc_driver);

MODULE_DESCRIPTION("Renesas RZ remote processor control driver");
MODULE_LICENSE("GPL v2");
