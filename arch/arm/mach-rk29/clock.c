/* arch/arm/mach-rk29/clock.c
 *
 * Copyright (C) 2010 ROCKCHIP, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

//#define DEBUG
#define pr_fmt(fmt) "clock: %s: " fmt, __func__

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/version.h>
#include <asm/clkdev.h>
#include <mach/rk29_iomap.h>
#include <mach/cru.h>

/* CRU PLL CON */
#define PLL_HIGH_BAND	(0x01 << 16)
#define PLL_LOW_BAND	(0x00 << 16)
#define PLL_PD		(0x01 << 15)

#define PLL_CLKR(i)	((((i) - 1) & 0x1f) << 10)
#define PLL_NR(v)	((((v) >> 10) & 0x1f) + 1)

#define PLL_CLKF(i)	((((i) - 1) & 0x7f) << 3)
#define PLL_NF(v)	((((v) >> 3) & 0x7f) + 1)
#define PLL_NF2(v)	(((((v) >> 3) & 0x7f) + 1) << 1)

#define PLL_CLKOD(i)	(((i) & 0x03) << 1)
#define PLL_NO_1	PLL_CLKOD(0)
#define PLL_NO_2	PLL_CLKOD(1)
#define PLL_NO_4	PLL_CLKOD(2)
#define PLL_NO_8	PLL_CLKOD(3)
#define PLL_NO_SHIFT(v)	(((v) >> 1) & 0x03)

#define PLL_BYPASS	(0x01)

/* CRU MODE CON */
#define CRU_CPU_MODE_MASK	(0x03u << 0)
#define CRU_CPU_MODE_SLOW	(0x00u << 0)
#define CRU_CPU_MODE_NORMAL	(0x01u << 0)
#define CRU_CPU_MODE_DSLOW	(0x02u << 0)

#define CRU_PERIPH_MODE_MASK	(0x03u << 2)
#define CRU_PERIPH_MODE_SLOW	(0x00u << 2)
#define CRU_PERIPH_MODE_NORMAL	(0x01u << 2)
#define CRU_PERIPH_MODE_DSLOW	(0x02u << 2)

#define CRU_CODEC_MODE_MASK	(0x03u << 4)
#define CRU_CODEC_MODE_SLOW	(0x00u << 4)
#define CRU_CODEC_MODE_NORMAL	(0x01u << 4)
#define CRU_CODEC_MODE_DSLOW	(0x02u << 4)

#define CRU_DDR_MODE_MASK	(0x03u << 6)
#define CRU_DDR_MODE_SLOW	(0x00u << 6)
#define CRU_DDR_MODE_NORMAL	(0x01u << 6)
#define CRU_DDR_MODE_DSLOW	(0x02u << 6)

/* Clock flags */
/* bit 0 is free */
#define RATE_FIXED		(1 << 1)	/* Fixed clock rate */
#define CONFIG_PARTICIPANT	(1 << 10)	/* Fundamental clock */
#define ENABLE_ON_INIT		(1 << 11)	/* Enable upon framework init */

#define cru_readl(offset)	readl(RK29_CRU_BASE + offset)
#define cru_writel(v, offset)	writel(v, RK29_CRU_BASE + offset)
#define cru_writel_force(v, offset)	do { u32 _v = v; u32 _count = 5; do { cru_writel(_v, offset); } while (cru_readl(offset) != _v && _count--); } while (0)	/* huangtao: when write CRU_xPLL_CON, first time may failed, so try again. unknown why. */

#define regfile_readl(offset)	readl(RK29_GRF_BASE + offset)

#define MHZ			(1000*1000)
#define KHZ			1000

struct clk {
	struct list_head	node;
	const char		*name;
	struct clk		*parent;
	struct list_head	children;
	struct list_head	sibling;	/* node for children */
	unsigned long		rate;
	u32			flags;
	int			(*mode)(struct clk *clk, int on);
	unsigned long		(*recalc)(struct clk *);	/* if null, follow parent */
	int			(*set_rate)(struct clk *, unsigned long);
	long			(*round_rate)(struct clk *, unsigned long);
	struct clk*		(*get_parent)(struct clk *);	/* get clk's parent from the hardware. default is clksel_get_parent if parents present */
	int			(*set_parent)(struct clk *, struct clk *);	/* default is clksel_set_parent if parents present */
	s32			usecount;
	u8			gate_idx;
	u8			pll_idx;
	u8			clksel_con;
	u8			clksel_mask;
	u8			clksel_shift;
	u8			clksel_maxdiv;
	u8			clksel_parent_mask;
	u8			clksel_parent_shift;
	struct clk		**parents;
};

static int clk_enable_nolock(struct clk *clk);
static void clk_disable_nolock(struct clk *clk);
static int clk_set_rate_nolock(struct clk *clk, unsigned long rate);
static int clk_set_parent_nolock(struct clk *clk, struct clk *parent);
static void __clk_reparent(struct clk *child, struct clk *parent);
static void __propagate_rate(struct clk *tclk);

static unsigned long clksel_recalc_div(struct clk *clk)
{
	u32 div = ((cru_readl(clk->clksel_con) >> clk->clksel_shift) & clk->clksel_mask) + 1;
	unsigned long rate = clk->parent->rate / div;
	pr_debug("%s new clock rate is %lu (div %u)\n", clk->name, rate, div);
	return rate;
}

static unsigned long clksel_recalc_shift(struct clk *clk)
{
	u32 shift = (cru_readl(clk->clksel_con) >> clk->clksel_shift) & clk->clksel_mask;
	unsigned long rate = clk->parent->rate >> shift;
	pr_debug("%s new clock rate is %lu (shift %u)\n", clk->name, rate, shift);
	return rate;
}

static unsigned long clksel_recalc_frac(struct clk *clk)
{
	unsigned long rate;
	u64 rate64;
	u32 r = cru_readl(clk->clksel_con), numerator, denominator;
	if (r == 0) // FPGA ?
		return clk->parent->rate;
	numerator = r >> 16;
	denominator = r & 0xFFFF;
	rate64 = (u64)clk->parent->rate * numerator;
	do_div(rate64, denominator);
	rate = rate64;
	pr_debug("%s new clock rate is %lu (frac %u/%u)\n", clk->name, rate, numerator, denominator);
	return rate;
}

static int clksel_set_rate_div(struct clk *clk, unsigned long rate)
{
	u32 div;

	for (div = 0; div <= clk->clksel_mask; div++) {
		u32 new_rate = clk->parent->rate / (div + 1);
		if (new_rate <= rate) {
			u32 v = cru_readl(clk->clksel_con);
			v &= ~((u32) clk->clksel_mask << clk->clksel_shift);
			v |= div << clk->clksel_shift;
			cru_writel(v, clk->clksel_con);
			clk->rate = new_rate;
			pr_debug("clksel_set_rate_div for clock %s to rate %ld (div %d)\n", clk->name, rate, div + 1);
			return 0;
		}
	}

	return -ENOENT;
}

static int clksel_set_rate_shift(struct clk *clk, unsigned long rate)
{
	u32 shift;

	for (shift = 0; (1 << shift) <= clk->clksel_maxdiv; shift++) {
		u32 new_rate = clk->parent->rate >> shift;
		if (new_rate <= rate) {
			u32 v = cru_readl(clk->clksel_con);
			v &= ~((u32) clk->clksel_mask << clk->clksel_shift);
			v |= shift << clk->clksel_shift;
			cru_writel(v, clk->clksel_con);
			clk->rate = new_rate;
			pr_debug("clksel_set_rate_shift for clock %s to rate %ld (shift %d)\n", clk->name, rate, shift);
			return 0;
		}
	}

	return -ENOENT;
}

static struct clk* clksel_get_parent(struct clk *clk)
{
	return clk->parents[(cru_readl(clk->clksel_con) >> clk->clksel_parent_shift) & clk->clksel_parent_mask];
}

static int clksel_set_parent(struct clk *clk, struct clk *parent)
{
	struct clk **p = clk->parents;
	u32 i;

	if (unlikely(!p))
		return -EINVAL;
	for (i = 0; (i <= clk->clksel_parent_mask) && *p; i++, p++) {
		u32 v;
		if (*p != parent)
			continue;
		v = cru_readl(clk->clksel_con);
		v &= ~((u32) clk->clksel_parent_mask << clk->clksel_parent_shift);
		v |= (i << clk->clksel_parent_shift);
		cru_writel(v, clk->clksel_con);
		return 0;
	}
	return -EINVAL;
}

static int gate_mode(struct clk *clk, int on)
{
	u32 reg;
	int idx = clk->gate_idx;
	u32 v;

	if (idx >= CLK_GATE_MAX)
		return -EINVAL;

	reg = CRU_CLKGATE0_CON;
	reg += (idx >> 5) << 2;
	idx &= 0x1F;

	v = cru_readl(reg);
	if (on) {
		v &= ~(1 << idx);	// clear bit 
	} else {
		v |= (1 << idx);	// set bit
	}
	cru_writel(v, reg);

	return 0;
}

static struct clk xin24m = {
	.name		= "xin24m",
	.rate		= 24 * MHZ,
	.flags		= RATE_FIXED,
};

static struct clk clk_12m = {
	.name		= "clk_12m",
	.rate		= 12 * MHZ,
	.parent		= &xin24m,
	.flags		= RATE_FIXED,
};

static struct clk extclk = {
	.name		= "extclk",
	.rate		= 27 * MHZ,
	.flags		= RATE_FIXED,
};

static struct clk otgphy0_clkin = {
	.name		= "otgphy0_clkin",
	.rate		= 480 * MHZ,
	.flags		= RATE_FIXED,
};

static struct clk otgphy1_clkin = {
	.name		= "otgphy1_clkin",
	.rate		= 480 * MHZ,
	.flags		= RATE_FIXED,
};


static void delay_500ns(void)
{
	int delay = 2000;
	while (delay--)
           barrier();
}


#define PERIPH_PLL_IDX     0
#define CODEC_PLL_IDX      1
#define ARM_PLL_IDX        2
#define DDR_PLL_IDX        3

#define GRF_SOC_CON0       0xbc
static void pll_wait_lock(int pll_idx, int delay)
{
	u32 bit = 0x2000000u << pll_idx;
	while (delay > 0) {
		if (regfile_readl(GRF_SOC_CON0) & bit)
			break;
		delay--;
	}
	if (delay == 0) {
		pr_warning("wait pll bit 0x%x time out!\n", bit);
	}
}

static unsigned long arm_pll_clk_recalc(struct clk *clk)
{
	unsigned long rate;

	if ((cru_readl(CRU_MODE_CON) & CRU_CPU_MODE_MASK) == CRU_CPU_MODE_NORMAL) {
		u32 v = cru_readl(CRU_APLL_CON);
		u64 rate64 = (u64) clk->parent->rate * PLL_NF2(v);
		do_div(rate64, PLL_NR(v));
		rate = rate64 >> PLL_NO_SHIFT(v);
		pr_debug("%s new clock rate is %ld (NF %d NR %d NO %d)\n", clk->name, rate, PLL_NF2(v), PLL_NR(v), 1 << PLL_NO_SHIFT(v));
	} else {
		rate = clk->parent->rate;
		pr_debug("%s new clock rate is %ld (slow mode)\n", clk->name, rate);
	}

	return rate;
}

struct arm_pll_set {
	u32	clk_hz;
	u32	pll_con;
	u32	clksel0_con;
};

#define CORE_ACLK_11	(0 << 5)
#define CORE_ACLK_21	(1 << 5)
#define CORE_ACLK_31	(2 << 5)
#define CORE_ACLK_41	(3 << 5)
#define CORE_ACLK_81	(4 << 5)
#define CORE_ACLK_MASK	(7 << 5)

#define ACLK_HCLK_11	(0 << 8)
#define ACLK_HCLK_21	(1 << 8)
#define ACLK_HCLK_41	(2 << 8)
#define ACLK_HCLK_MASK	(3 << 8)

#define ACLK_PCLK_11	(0 << 10)
#define ACLK_PCLK_21	(1 << 10)
#define ACLK_PCLK_41	(2 << 10)
#define ACLK_PCLK_81	(3 << 10)
#define ACLK_PCLK_MASK	(3 << 10)

#define ARM_PLL(_clk_mhz, nr, nf, no, _axi_div, _ahb_div, _apb_div) \
{ \
	.clk_hz		= _clk_mhz * MHZ, \
	.pll_con	= PLL_CLKR(nr) | PLL_CLKF(nf >> 1) | PLL_NO_##no, \
	.clksel0_con	= CORE_ACLK_##_axi_div | ACLK_HCLK_##_ahb_div | ACLK_PCLK_##_apb_div, \
}

static const struct arm_pll_set arm_pll[] = {
	// clk_mhz = 24 * NF / (NR * NO)
	//      mhz  NR NF NO adiv hdiv pdiv
//	ARM_PLL(600, 1, 50, 2, 21, 21, 41),
//	ARM_PLL(624, 1, 52, 2, 21, 21, 41),
	ARM_PLL(720, 1, 60, 2, 21, 21, 41),
	// last item, pll power down.
	ARM_PLL( 24, 1, 64, 8, 21, 21, 41),
};

#define CORE_PARENT_MASK	(3 << 23)
#define CORE_PARENT_ARM_PLL	(0 << 23)
#define CORE_PARENT_PERIPH_PLL	(1 << 23)

static int arm_pll_clk_set_rate(struct clk *clk, unsigned long rate)
{
	u32 v = arm_pll[0].pll_con;

	/* make aclk safe & reparent to periph pll */
	cru_writel((cru_readl(CRU_CLKSEL0_CON) & ~(CORE_PARENT_MASK | CORE_ACLK_MASK)) | CORE_PARENT_PERIPH_PLL | CORE_ACLK_21, CRU_CLKSEL0_CON);

	/* enter slow mode */
	cru_writel((cru_readl(CRU_MODE_CON) & ~CRU_CPU_MODE_MASK) | CRU_CPU_MODE_SLOW, CRU_MODE_CON);

	pll_wait_lock(ARM_PLL_IDX, 2400000);

	/* power down */
	cru_writel(cru_readl(CRU_APLL_CON) | PLL_PD, CRU_APLL_CON);

	delay_500ns();

	cru_writel(v | PLL_PD, CRU_APLL_CON);

	delay_500ns();

	/* power up */
	cru_writel(v, CRU_APLL_CON);

	pll_wait_lock(ARM_PLL_IDX, 2400000);

	/* enter normal mode */
	cru_writel((cru_readl(CRU_MODE_CON) & ~CRU_CPU_MODE_MASK) | CRU_CPU_MODE_NORMAL, CRU_MODE_CON);

	/* reparent to arm pll & set aclk/hclk/pclk */
	cru_writel((cru_readl(CRU_CLKSEL0_CON) & ~(CORE_PARENT_MASK | CORE_ACLK_MASK | ACLK_HCLK_MASK | ACLK_PCLK_MASK)) | CORE_PARENT_ARM_PLL | arm_pll[0].clksel0_con, CRU_CLKSEL0_CON);

	return 0;
}

static struct clk arm_pll_clk = {
	.name		= "arm_pll",
	.parent		= &xin24m,
	.recalc		= arm_pll_clk_recalc,
	.set_rate	= arm_pll_clk_set_rate,
};

static unsigned long ddr_pll_clk_recalc(struct clk *clk)
{
	unsigned long rate;

	if ((cru_readl(CRU_MODE_CON) & CRU_DDR_MODE_MASK) == CRU_DDR_MODE_NORMAL) {
		u32 v = cru_readl(CRU_DPLL_CON);
		u64 rate64 = (u64) clk->parent->rate * PLL_NF(v);
		do_div(rate64, PLL_NR(v));
		rate = rate64 >> PLL_NO_SHIFT(v);
		pr_debug("%s new clock rate is %ld (NF %d NR %d NO %d)\n", clk->name, rate, PLL_NF(v), PLL_NR(v), 1 << PLL_NO_SHIFT(v));
	} else {
		rate = clk->parent->rate;
		pr_debug("%s new clock rate is %ld (slow mode)\n", clk->name, rate);
	}

	return rate;
}

static struct clk ddr_pll_clk = {
	.name		= "ddr_pll",
	.parent		= &xin24m,
	.recalc		= ddr_pll_clk_recalc,
};


static unsigned long codec_pll_clk_recalc(struct clk *clk)
{
	unsigned long rate;

	if ((cru_readl(CRU_MODE_CON) & CRU_CODEC_MODE_MASK) == CRU_CODEC_MODE_NORMAL) {
		u32 v = cru_readl(CRU_CPLL_CON);
		u64 rate64 = (u64) clk->parent->rate * PLL_NF(v);
		do_div(rate64, PLL_NR(v));
		rate = rate64 >> PLL_NO_SHIFT(v);
		pr_debug("%s new clock rate is %ld (NF %d NR %d NO %d)\n", clk->name, rate, PLL_NF(v), PLL_NR(v), 1 << PLL_NO_SHIFT(v));
	} else {
		rate = clk->parent->rate;
		pr_debug("%s new clock rate is %ld (slow mode)\n", clk->name, rate);
	}

	return rate;
}

static struct clk codec_pll_clk = {
	.name		= "codec_pll",
	.parent		= &xin24m,
	.recalc		= codec_pll_clk_recalc,
};


static unsigned long periph_pll_clk_recalc(struct clk *clk)
{
	unsigned long rate;

	if ((cru_readl(CRU_MODE_CON) & CRU_PERIPH_MODE_MASK) == CRU_PERIPH_MODE_NORMAL) {
		u32 v = cru_readl(CRU_PPLL_CON);
		u64 rate64 = (u64) clk->parent->rate * PLL_NF(v);
		do_div(rate64, PLL_NR(v));
		rate = rate64 >> PLL_NO_SHIFT(v);
		pr_debug("%s new clock rate is %ld (NF %d NR %d NO %d)\n", clk->name, rate, PLL_NF(v), PLL_NR(v), 1 << PLL_NO_SHIFT(v));
	} else {
		rate = clk->parent->rate;
		pr_debug("%s new clock rate is %ld (slow mode)\n", clk->name, rate);
	}

	return rate;
}

static int periph_pll_clk_set_rate(struct clk *clk, unsigned long rate)
{
	/* 624M: high-band, NR=1, NF=26, NO=1 */
	u32 v = PLL_HIGH_BAND | PLL_CLKR(1) | PLL_CLKF(26) | PLL_NO_1;

	/* enter slow mode */
	cru_writel((cru_readl(CRU_MODE_CON) & ~CRU_PERIPH_MODE_MASK) | CRU_PERIPH_MODE_SLOW, CRU_MODE_CON);

	pll_wait_lock(PERIPH_PLL_IDX, 2400000);

	/* power down */
	cru_writel(cru_readl(CRU_PPLL_CON) | PLL_PD, CRU_PPLL_CON);

	delay_500ns();

	cru_writel(v | PLL_PD, CRU_PPLL_CON);

	delay_500ns();

	/* power up */
	cru_writel(v, CRU_PPLL_CON);

	pll_wait_lock(PERIPH_PLL_IDX, 2400000);

	/* enter normal mode */
	cru_writel((cru_readl(CRU_MODE_CON) & ~CRU_PERIPH_MODE_MASK) | CRU_PERIPH_MODE_NORMAL, CRU_MODE_CON);

	return 0;
}

static struct clk periph_pll_clk = {
	.name		= "periph_pll",
	.parent		= &xin24m,
	.recalc		= periph_pll_clk_recalc,
	.set_rate	= periph_pll_clk_set_rate,
};


static struct clk clk_core = {
	.name		= "core",
	.parent		= &arm_pll_clk,
	.recalc		= clksel_recalc_div,
	.clksel_con	= CRU_CLKSEL0_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 0,
};

static unsigned long aclk_cpu_recalc(struct clk *clk)
{
	unsigned long rate;
	u32 div = ((cru_readl(CRU_CLKSEL0_CON) >> 5) & 0x7) + 1;

	BUG_ON(div > 5);
	if (div >= 5)
		div = 8;
	rate = clk->parent->rate / div;
	pr_debug("%s new clock rate is %ld (div %d)\n", clk->name, rate, div);

	return rate;
}

static struct clk aclk_cpu = {
	.name		= "aclk_cpu",
	.parent		= &clk_core,
	.recalc		= aclk_cpu_recalc,
};

static struct clk hclk_cpu = {
	.name		= "hclk_cpu",
	.parent		= &aclk_cpu,
	.recalc		= clksel_recalc_shift,
	.clksel_con	= CRU_CLKSEL0_CON,
	.clksel_mask	= 3,
	.clksel_shift	= 8,
	.clksel_maxdiv	= 4,
};

static struct clk pclk_cpu = {
	.name		= "pclk_cpu",
	.parent		= &aclk_cpu,
	.recalc		= clksel_recalc_shift,
	.clksel_con	= CRU_CLKSEL0_CON,
	.clksel_mask	= 3,
	.clksel_shift	= 10,
	.clksel_maxdiv	= 8,
};

static struct clk aclk_periph = {
	.name		= "aclk_periph",
	.parent		= &periph_pll_clk,
	.recalc		= clksel_recalc_div,
	.clksel_con	= CRU_CLKSEL0_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 14,
};

static struct clk pclk_periph = {
	.name		= "pclk_periph",
	.parent		= &aclk_periph,
	.recalc		= clksel_recalc_shift,
	.clksel_con	= CRU_CLKSEL0_CON,
	.clksel_mask	= 3,
	.clksel_shift	= 19,
	.clksel_maxdiv	= 8,
};

static struct clk hclk_periph = {
	.name		= "hclk_periph",
	.parent		= &aclk_periph,
	.recalc		= clksel_recalc_shift,
	.clksel_con	= CRU_CLKSEL0_CON,
	.clksel_mask	= 3,
	.clksel_shift	= 21,
	.clksel_maxdiv	= 4,
};


static struct clk *clk_uhost_parents[8] = { &periph_pll_clk, &ddr_pll_clk, &codec_pll_clk, &arm_pll_clk, &otgphy0_clkin, &otgphy1_clkin };

static struct clk clk_uhost = {
	.name		= "uhost",
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GATE_UHOST,
	.clksel_con	= CRU_CLKSEL1_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 16,
	.clksel_parent_mask	= 7,
	.clksel_parent_shift	= 13,
	.parents	= clk_uhost_parents,
};

static struct clk *clk_otgphy_parents[4] = { &xin24m, &clk_12m, &clk_uhost };

static struct clk clk_otgphy0 = {
	.name		= "otgphy0",
	.clksel_con	= CRU_CLKSEL1_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 9,
	.parents	= clk_otgphy_parents,
};

static struct clk clk_otgphy1 = {
	.name		= "otgphy1",
	.clksel_con	= CRU_CLKSEL1_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 11,
	.parents	= clk_otgphy_parents,
};


static struct clk rmii_clkin = {
	.name		= "rmii_clkin",
};

static struct clk *clk_mac_ref_div_parents[4] = { &arm_pll_clk, &periph_pll_clk, &codec_pll_clk, &ddr_pll_clk };

static struct clk clk_mac_ref_div = {
	.name		= "mac_ref_div",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL1_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 23,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 21,
	.parents	= clk_mac_ref_div_parents,
};

static struct clk *clk_mac_ref_parents[2] = { &clk_mac_ref_div, &rmii_clkin };

static struct clk clk_mac_ref = {
	.name		= "mac_ref",
	.clksel_con	= CRU_CLKSEL1_CON,
	.clksel_parent_mask	= 1,
	.clksel_parent_shift	= 28,
	.parents	= clk_mac_ref_parents,
};


static struct clk *clk_i2s_div_parents[8] = { &codec_pll_clk, &periph_pll_clk, &arm_pll_clk, &ddr_pll_clk, &otgphy0_clkin, &otgphy1_clkin };

static struct clk clk_i2s0_div = {
	.name		= "i2s0_div",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL2_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 3,
	.clksel_parent_mask	= 7,
	.clksel_parent_shift	= 0,
	.parents	= clk_i2s_div_parents,
};

static struct clk clk_i2s1_div = {
	.name		= "i2s1_div",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL2_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 13,
	.clksel_parent_mask	= 7,
	.clksel_parent_shift	= 10,
	.parents	= clk_i2s_div_parents,
};

static struct clk clk_spdif_div = {
	.name		= "spdif_div",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL2_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 23,
	.clksel_parent_mask	= 7,
	.clksel_parent_shift	= 20,
	.parents	= clk_i2s_div_parents,
};

static int clk_i2s_frac_div_set_rate(struct clk *clk, unsigned long rate)
{
	u16 numerator, denominator;

	switch (rate) {
	case 8192000:	/* 624*128/9750 */
		numerator = 128;
		denominator = 9750;
		break;
	case 11289600:	/* 624*294/16250 */
		numerator = 294;
		denominator = 16250;
		break;
	case 12288000:	/* 624*64/3250 */
		numerator = 64;
		denominator = 3250;
		break;
	case 22579200:	/* 624*294/8125 */
		numerator = 294;
		denominator = 8125;
		break;
	case 24576000:	/* 624*64/1625 */
		numerator = 64;
		denominator = 1625;
		break;
	case 45158400:	/* 624*588/8125 */
		numerator = 588;
		denominator = 8125;
		break;
	case 49152000:	/* 624*128/1625 */
		numerator = 128;
		denominator = 1625;
		break;
	default:
		return -ENOENT;
	}

	pr_debug("set clock %s to rate %ld (%d/%d)\n", clk->name, rate, numerator, denominator);
	cru_writel((u32)numerator << 16 | denominator, clk->clksel_con);

	return 0;
}

static struct clk clk_i2s0_frac_div = {
	.name		= "i2s0_frac_div",
	.parent		= &clk_i2s0_div,
	.recalc		= clksel_recalc_frac,
	.set_rate	= clk_i2s_frac_div_set_rate,
	.clksel_con	= CRU_CLKSEL3_CON,
};

static struct clk clk_i2s1_frac_div = {
	.name		= "i2s1_frac_div",
	.parent		= &clk_i2s1_div,
	.recalc		= clksel_recalc_frac,
	.set_rate	= clk_i2s_frac_div_set_rate,
	.clksel_con	= CRU_CLKSEL4_CON,
};

static struct clk clk_spdif_frac_div = {
	.name		= "spdif_frac_div",
	.parent		= &clk_spdif_div,
	.recalc		= clksel_recalc_frac,
	.set_rate	= clk_i2s_frac_div_set_rate,
	.clksel_con	= CRU_CLKSEL5_CON,
};

static int i2s_set_rate(struct clk *clk, unsigned long rate)
{
	int ret;
	struct clk *parent;

	if (rate == 12 * MHZ) {
		parent = &clk_12m;
	} else {
		parent = clk->parents[1]; /* frac div */
		ret = clk_set_rate_nolock(parent, rate);
		if (ret)
			return ret;
	}
	if (clk->parent != parent)
		clk_set_parent_nolock(clk, parent);

	return ret;
}

static struct clk *clk_i2s0_parents[4] = { &clk_i2s0_div, &clk_i2s0_frac_div, &clk_12m, &xin24m };

static struct clk clk_i2s0 = {
	.name		= "i2s0",
	.set_rate	= i2s_set_rate,
	.clksel_con	= CRU_CLKSEL2_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 8,
	.parents	= clk_i2s0_parents,
};

static struct clk *clk_i2s1_parents[4] = { &clk_i2s1_div, &clk_i2s1_frac_div, &clk_12m, &xin24m };

static struct clk clk_i2s1 = {
	.name		= "i2s1",
	.set_rate	= i2s_set_rate,
	.clksel_con	= CRU_CLKSEL2_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 18,
	.parents	= clk_i2s1_parents,
};

static struct clk *clk_spdif_parents[4] = { &clk_spdif_div, &clk_spdif_frac_div, &clk_12m, &xin24m };

static struct clk clk_spdif = {
	.name		= "spdif",
	.set_rate	= i2s_set_rate,
	.clksel_con	= CRU_CLKSEL2_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 28,
	.parents	= clk_spdif_parents,
};


static struct clk *clk_spi_src_parents[4] = { &periph_pll_clk, &ddr_pll_clk, &codec_pll_clk, &arm_pll_clk };

static struct clk clk_spi_src = {
	.name		= "spi_src",
	.clksel_con	= CRU_CLKSEL6_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 0,
	.parents	= clk_spi_src_parents,
};

static struct clk clk_spi0 = {
	.name		= "spi0",
	.parent		= &clk_spi_src,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GATE_SPI0,
	.clksel_con	= CRU_CLKSEL6_CON,
	.clksel_mask	= 0x7F,
	.clksel_shift	= 2,
};

static struct clk clk_spi1 = {
	.name		= "spi1",
	.parent		= &clk_spi_src,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GATE_SPI1,
	.clksel_con	= CRU_CLKSEL6_CON,
	.clksel_mask	= 0x7F,
	.clksel_shift	= 11,
};


static struct clk clk_saradc = {
	.name		= "saradc",
	.parent		= &pclk_periph,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GATE_SARADC,
	.clksel_con	= CRU_CLKSEL6_CON,
	.clksel_mask	= 0xFF,
	.clksel_shift	= 18,
};


static struct clk *clk_cpu_timer_parents[2] = { &pclk_cpu, &xin24m };

static struct clk clk_timer0 = {
	.name		= "timer0",
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_TIMER0,
	.clksel_con	= CRU_CLKSEL6_CON,
	.clksel_parent_mask	= 1,
	.clksel_parent_shift	= 26,
	.parents	= clk_cpu_timer_parents,
};

static struct clk clk_timer1 = {
	.name		= "timer1",
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_TIMER1,
	.clksel_con	= CRU_CLKSEL6_CON,
	.clksel_parent_mask	= 1,
	.clksel_parent_shift	= 27,
	.parents	= clk_cpu_timer_parents,
};

static struct clk *clk_periph_timer_parents[2] = { &pclk_periph, &xin24m };

static struct clk clk_timer2 = {
	.name		= "timer2",
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_TIMER2,
	.clksel_con	= CRU_CLKSEL6_CON,
	.clksel_parent_mask	= 1,
	.clksel_parent_shift	= 28,
	.parents	= clk_periph_timer_parents,
};

static struct clk clk_timer3 = {
	.name		= "timer3",
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_TIMER3,
	.clksel_con	= CRU_CLKSEL6_CON,
	.clksel_parent_mask	= 1,
	.clksel_parent_shift	= 29,
	.parents	= clk_periph_timer_parents,
};


static struct clk *clk_sdmmc_src_parents[4] = { &arm_pll_clk, &periph_pll_clk, &codec_pll_clk, &ddr_pll_clk };

static struct clk clk_sdmmc_src = {
	.name		= "sdmmc_src",
	.clksel_con	= CRU_CLKSEL7_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 0,
	.parents	= clk_sdmmc_src_parents,
};

static struct clk clk_sdmmc0 = {
	.name		= "sdmmc0",
	.parent		= &clk_sdmmc_src,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GATE_SDMMC0,
	.clksel_con	= CRU_CLKSEL7_CON,
	.clksel_mask	= 0x3F,
	.clksel_shift	= 2,
};

static struct clk clk_sdmmc1 = {
	.name		= "sdmmc1",
	.parent		= &clk_sdmmc_src,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GATE_SDMMC1,
	.clksel_con	= CRU_CLKSEL7_CON,
	.clksel_mask	= 0x3F,
	.clksel_shift	= 10,
};

static struct clk clk_emmc = {
	.name		= "emmc",
	.parent		= &clk_sdmmc_src,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GATE_EMMC,
	.clksel_con	= CRU_CLKSEL7_CON,
	.clksel_mask	= 0x3F,
	.clksel_shift	= 18,
};

static struct clk clk_sdmmc0_ahb = {
	.name		= "sdmmc0_ahb",
	.parent		= &hclk_periph,
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_SDMMC0_AHB,
};

static struct clk clk_sdmmc1_ahb = {
	.name		= "sdmmc1_ahb",
	.parent		= &hclk_periph,
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_SDMMC1_AHB,
};

static struct clk clk_emmc_ahb = {
	.name		= "emmc_ahb",
	.parent		= &hclk_periph,
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_EMMC_AHB,
};


static struct clk *clk_ddr_parents[8] = { &ddr_pll_clk, &periph_pll_clk, &codec_pll_clk, &arm_pll_clk };

static struct clk clk_ddr = {
	.name		= "ddr",
	.recalc		= clksel_recalc_shift,
	.clksel_con	= CRU_CLKSEL7_CON,
	.clksel_mask	= 7,
	.clksel_shift	= 26,
	.clksel_maxdiv	= 32,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 24,
	.parents	= clk_ddr_parents,
};


static int clk_uart_set_rate(struct clk *clk, unsigned long rate)
{
	int ret;
	struct clk *parent;
	struct clk *clk_div = clk->parents[0];

	switch (rate) {
	case 24*MHZ: /* 1.5M/0.5M/50/75/150/200/300/600/1200/2400 */
		parent = clk->parents[2]; /* xin24m */
		break;
	case 9600*16:
	case 19200*16:
	case 38400*16:
	case 57600*16:
	case 115200*16:
	case 230400*16:
	case 460800*16:
	case 576000*16:
	case 921600*16:
	case 1152000*16:
		parent = clk->parents[1]; /* frac div */
		/* reset div to 1 */
		ret = clk_set_rate_nolock(clk_div, clk_div->parent->rate);
		if (ret)
			return ret;
		break;
	default:
		parent = clk_div;
		break;
	}

	if (parent->set_rate) {
		ret = clk_set_rate_nolock(parent, rate);
		if (ret)
			return ret;
	}

	if (clk->parent != parent)
		clk_set_parent_nolock(clk, parent);

	return 0;
}

static int clk_uart_frac_div_set_rate(struct clk *clk, unsigned long rate)
{
	u16 numerator, denominator;

	switch (rate) {
	case 9600*16:
		numerator = 2;
		denominator = 8125;
		break;
	case 19200*16:
		numerator = 4;
		denominator = 8125;
		break;
	case 38400*16:
		numerator = 8;
		denominator = 8125;
		break;
	case 57600*16:
		numerator = 12;
		denominator = 8125;
		break;
	case 115200*16:
		numerator = 24;
		denominator = 8125;
		break;
	case 230400*16:
		numerator = 48;
		denominator = 8125;
		break;
	case 460800*16:
		numerator = 96;
		denominator = 8125;
		break;
	case 576000*16:
		numerator = 24;
		denominator = 1625;
		break;
	case 921600*16:
		numerator = 192;
		denominator = 8125;
		break;
	case 1152000*16:
		numerator = 48;
		denominator = 1625;
		break;
	default:
		return -ENOENT;
	}

	pr_debug("set clock %s to rate %ld (%d/%d)\n", clk->name, rate, numerator, denominator);
	cru_writel((u32)numerator << 16 | denominator, clk->clksel_con);

	return 0;
}
static struct clk *clk_uart_src_parents[8] = { &periph_pll_clk, &ddr_pll_clk, &codec_pll_clk, &arm_pll_clk, &otgphy0_clkin, &otgphy1_clkin };

static struct clk clk_uart01_src = {
	.name		= "uart01_src",
	.clksel_con	= CRU_CLKSEL8_CON,
	.clksel_parent_mask	= 7,
	.clksel_parent_shift	= 0,
	.parents	= clk_uart_src_parents,
};

static struct clk clk_uart0_div = {
	.name		= "uart0_div",
	.parent		= &clk_uart01_src,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL8_CON,
	.clksel_mask	= 0x3F,
	.clksel_shift	= 3,
};

static struct clk clk_uart0_frac_div = {
	.name		= "uart0_frac_div",
	.parent		= &clk_uart0_div,
	.recalc		= clksel_recalc_frac,
	.set_rate	= clk_uart_frac_div_set_rate,
	.clksel_con	= CRU_CLKSEL10_CON,
};

static struct clk *clk_uart0_parents[4] = { &clk_uart0_div, &clk_uart0_frac_div, &xin24m };

static struct clk clk_uart0 = {
	.name		= "uart0",
	.mode		= gate_mode,
	.set_rate	= clk_uart_set_rate,
	.gate_idx	= CLK_GATE_UART0,
	.clksel_con	= CRU_CLKSEL8_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 9,
	.parents	= clk_uart0_parents,
};

static struct clk clk_uart1_div = {
	.name		= "uart1_div",
	.parent		= &clk_uart01_src,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL8_CON,
	.clksel_mask	= 0x3F,
	.clksel_shift	= 14,
};

static struct clk clk_uart1_frac_div = {
	.name		= "uart1_frac_div",
	.parent		= &clk_uart1_div,
	.recalc		= clksel_recalc_frac,
	.set_rate	= clk_uart_frac_div_set_rate,
	.clksel_con	= CRU_CLKSEL11_CON,
};

static struct clk *clk_uart1_parents[4] = { &clk_uart1_div, &clk_uart1_frac_div, &xin24m };

static struct clk clk_uart1 = {
	.name		= "uart1",
	.mode		= gate_mode,
	.set_rate	= clk_uart_set_rate,
	.gate_idx	= CLK_GATE_UART1,
	.clksel_con	= CRU_CLKSEL8_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 20,
	.parents	= clk_uart1_parents,
};

static struct clk clk_uart23_src = {
	.name		= "uart23_src",
	.clksel_con	= CRU_CLKSEL9_CON,
	.clksel_parent_mask	= 7,
	.clksel_parent_shift	= 0,
	.parents	= clk_uart_src_parents,
};

static struct clk clk_uart2_div = {
	.name		= "uart2_div",
	.parent		= &clk_uart23_src,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL9_CON,
	.clksel_mask	= 0x3F,
	.clksel_shift	= 3,
};

static struct clk clk_uart2_frac_div = {
	.name		= "uart2_frac_div",
	.parent		= &clk_uart2_div,
	.recalc		= clksel_recalc_frac,
	.set_rate	= clk_uart_frac_div_set_rate,
	.clksel_con	= CRU_CLKSEL12_CON,
};

static struct clk *clk_uart2_parents[4] = { &clk_uart2_div, &clk_uart2_frac_div, &xin24m };

static struct clk clk_uart2 = {
	.name		= "uart2",
	.mode		= gate_mode,
	.set_rate	= clk_uart_set_rate,
	.gate_idx	= CLK_GATE_UART2,
	.clksel_con	= CRU_CLKSEL9_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 9,
	.parents	= clk_uart2_parents,
};

static struct clk clk_uart3_div = {
	.name		= "uart3_div",
	.parent		= &clk_uart23_src,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL9_CON,
	.clksel_mask	= 0x3F,
	.clksel_shift	= 14,
};

static struct clk clk_uart3_frac_div = {
	.name		= "uart3_frac_div",
	.parent		= &clk_uart3_div,
	.recalc		= clksel_recalc_frac,
	.set_rate	= clk_uart_frac_div_set_rate,
	.clksel_con	= CRU_CLKSEL13_CON,
};

static struct clk *clk_uart3_parents[4] = { &clk_uart3_div, &clk_uart3_frac_div, &xin24m };

static struct clk clk_uart3 = {
	.name		= "uart3",
	.mode		= gate_mode,
	.set_rate	= clk_uart_set_rate,
	.gate_idx	= CLK_GATE_UART3,
	.clksel_con	= CRU_CLKSEL9_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 20,
	.parents	= clk_uart3_parents,
};


static struct clk *clk_hsadc_div_parents[8] = { &codec_pll_clk, &ddr_pll_clk, &periph_pll_clk, &arm_pll_clk, &otgphy0_clkin, &otgphy1_clkin };

static struct clk clk_hsadc_div = {
	.name		= "hsadc_div",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL14_CON,
	.clksel_mask	= 0xFF,
	.clksel_shift	= 10,
	.clksel_parent_mask	= 7,
	.clksel_parent_shift	= 7,
	.parents	= clk_hsadc_div_parents,
};

static struct clk clk_hsadc_frac_div = {
	.name		= "hsadc_frac_div",
	.parent		= &clk_hsadc_div,
	.recalc		= clksel_recalc_frac,
	.clksel_con	= CRU_CLKSEL15_CON,
};

static struct clk *clk_demod_parents[4] = { &clk_hsadc_div, &clk_hsadc_frac_div, &extclk };

static struct clk clk_demod = {
	.name		= "demod",
	.clksel_con	= CRU_CLKSEL14_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 18,
	.parents	= clk_demod_parents,
};

static struct clk gpsclk = {
	.name		= "gpsclk",
};

static struct clk *clk_hsadc_parents[2] = { &clk_demod, &gpsclk };

static struct clk clk_hsadc = {
	.name		= "hsadc",
	.clksel_con	= CRU_CLKSEL14_CON,
	.clksel_parent_mask	= 1,
	.clksel_parent_shift	= 21,
	.parents	= clk_hsadc_parents,
};

static unsigned long div2_recalc(struct clk *clk)
{
	return clk->parent->rate >> 1;
}

static struct clk clk_hsadc_div2 = {
	.name		= "hsadc_div2",
	.parent		= &clk_demod,
	.recalc		= div2_recalc,
};

static struct clk clk_hsadc_div2_inv = {
	.name		= "hsadc_div2_inv",
	.parent		= &clk_demod,
	.recalc		= div2_recalc,
};

static struct clk *clk_hsadc_out_parents[2] = { &clk_hsadc_div2, &clk_hsadc_div2_inv };

static struct clk clk_hsadc_out = {
	.name		= "hsadc_out",
	.clksel_con	= CRU_CLKSEL14_CON,
	.clksel_parent_mask	= 1,
	.clksel_parent_shift	= 20,
	.parents	= clk_hsadc_out_parents,
};


static struct clk *dclk_lcdc_div_parents[4] = { &codec_pll_clk, &ddr_pll_clk, &periph_pll_clk, &arm_pll_clk };

static struct clk dclk_lcdc_div = {
	.name		= "dclk_lcdc_div",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL16_CON,
	.clksel_mask	= 0xFF,
	.clksel_shift	= 2,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 0,
	.parents	= dclk_lcdc_div_parents,
};

static struct clk *dclk_lcdc_parents[2] = { &dclk_lcdc_div, &extclk };

static struct clk dclk_lcdc = {
	.name		= "dclk_lcdc",
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_LCDC,
	.clksel_con	= CRU_CLKSEL16_CON,
	.clksel_parent_mask	= 1,
	.clksel_parent_shift	= 10,
	.parents	= dclk_lcdc_parents,
};

static struct clk dclk_ebook = {
	.name		= "dclk_ebook",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL16_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 13,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 11,
	.parents	= dclk_lcdc_div_parents,
};

static struct clk *aclk_lcdc_parents[4] = { &ddr_pll_clk, &codec_pll_clk, &periph_pll_clk, &arm_pll_clk };

static struct clk aclk_lcdc = {
	.name		= "aclk_lcdc",
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_LCDC_AXI,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL16_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 20,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 18,
	.parents	= aclk_lcdc_parents,
};

static struct clk hclk_lcdc = {
	.name		= "hclk_lcdc",
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_LCDC_AHB,
	.parent		= &aclk_lcdc,
	.clksel_con	= CRU_CLKSEL16_CON,
	.recalc		= clksel_recalc_shift,
	.set_rate	= clksel_set_rate_shift,
	.clksel_mask	= 3,
	.clksel_shift	= 25,
	.clksel_maxdiv	= 4,
};

static struct clk *xpu_parents[4] = { &periph_pll_clk, &ddr_pll_clk, &codec_pll_clk, &arm_pll_clk };

static struct clk aclk_vepu = {
	.name		= "aclk_vepu",
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GAET_VEPU_AXI,
	.clksel_con	= CRU_CLKSEL17_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 2,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 0,
	.parents	= xpu_parents,
};

static struct clk hclk_vepu = {
	.name		= "hclk_vepu",
	.parent		= &aclk_vepu,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_shift,
	.set_rate	= clksel_set_rate_shift,
	.gate_idx	= CLK_GATE_VEPU_AHB,
	.clksel_con	= CRU_CLKSEL17_CON,
	.clksel_mask	= 3,
	.clksel_shift	= 28,
	.clksel_maxdiv	= 4,
};

static struct clk aclk_vdpu = {
	.name		= "aclk_vdpu",
	.parent		= &periph_pll_clk,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.gate_idx	= CLK_GATE_VDPU_AXI,
	.clksel_con	= CRU_CLKSEL17_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 9,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 7,
	.parents	= xpu_parents,
};

static struct clk hclk_vdpu = {
	.name		= "hclk_vdpu",
	.parent		= &aclk_vdpu,
	.mode		= gate_mode,
	.recalc		= clksel_recalc_shift,
	.set_rate	= clksel_set_rate_shift,
	.gate_idx	= CLK_GATE_VDPU_AHB,
	.clksel_con	= CRU_CLKSEL17_CON,
	.clksel_mask	= 3,
	.clksel_shift	= 30,
	.clksel_maxdiv	= 4,
};

static struct clk clk_gpu = {
	.name		= "gpu",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL17_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 16,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 14,
	.parents	= xpu_parents,
};

static struct clk aclk_gpu = {
	.name		= "aclk_gpu",
	.recalc		= clksel_recalc_div,
	.set_rate	= clksel_set_rate_div,
	.clksel_con	= CRU_CLKSEL17_CON,
	.clksel_mask	= 0x1F,
	.clksel_shift	= 23,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 21,
	.parents	= xpu_parents,
};


static struct clk *clk_vip_parents[4] = { &xin24m, &extclk, &dclk_ebook };

static struct clk clk_vip = {
	.name		= "vip",
	.mode		= gate_mode,
	.gate_idx	= CLK_GATE_VIP,
	.clksel_con	= CRU_CLKSEL1_CON,
	.clksel_parent_mask	= 3,
	.clksel_parent_shift	= 7,
	.parents	= clk_vip_parents,
};


#define GATE_CLK(NAME,PARENT,ID) \
static struct clk clk_##NAME = { \
	.name		= #NAME, \
	.parent		= &PARENT, \
	.mode		= gate_mode, \
	.gate_idx	= CLK_GATE_##ID, \
}

GATE_CLK(i2c0, pclk_cpu, I2C0);
GATE_CLK(i2c1, pclk_periph, I2C1);
GATE_CLK(i2c2, pclk_periph, I2C2);
GATE_CLK(i2c3, pclk_periph, I2C3);

GATE_CLK(gpio0, pclk_cpu, GPIO0);
GATE_CLK(gpio1, pclk_periph, GPIO1);
GATE_CLK(gpio2, pclk_periph, GPIO2);
GATE_CLK(gpio3, pclk_periph, GPIO3);
GATE_CLK(gpio4, pclk_cpu, GPIO4);
GATE_CLK(gpio5, pclk_periph, GPIO5);
GATE_CLK(gpio6, pclk_cpu, GPIO6);

#define CLK(dev, con, ck) \
	{ \
		.dev_id = dev, \
		.con_id = con, \
		.clk = ck, \
	}

#define CLK1(name) \
	{ \
		.dev_id = NULL, \
		.con_id = #name, \
		.clk = &clk_##name, \
	}

static struct clk_lookup clks[] = {
	CLK(NULL, "xin24m", &xin24m),
	CLK(NULL, "extclk", &extclk),
	CLK(NULL, "otgphy0_clkin", &otgphy0_clkin),
	CLK(NULL, "otgphy1_clkin", &otgphy1_clkin),
	CLK(NULL, "gpsclk", &gpsclk),

	CLK1(12m),
	CLK(NULL, "arm_pll", &arm_pll_clk),
	CLK(NULL, "ddr_pll", &ddr_pll_clk),
	CLK(NULL, "codec_pll", &codec_pll_clk),
	CLK(NULL, "periph_pll", &periph_pll_clk),

	CLK1(core),
	CLK(NULL, "aclk_cpu", &aclk_cpu),
	CLK(NULL, "hclk_cpu", &hclk_cpu),
	CLK(NULL, "pclk_cpu", &pclk_cpu),

	CLK(NULL, "aclk_periph", &aclk_periph),
	CLK(NULL, "hclk_periph", &hclk_periph),
	CLK(NULL, "pclk_periph", &pclk_periph),

	CLK1(vip),
	CLK("rk29_otgphy.0", "otgphy", &clk_otgphy0),
	CLK("rk29_otgphy.1", "otgphy", &clk_otgphy1),
	CLK(NULL, "uhost", &clk_uhost),
	CLK(NULL, "mac_ref_div", &clk_mac_ref_div),
	CLK(NULL, "mac_ref", &clk_mac_ref),

	CLK("rk29_i2s.0", "i2s_div", &clk_i2s0_div),
	CLK("rk29_i2s.0", "i2s_frac_div", &clk_i2s0_frac_div),
	CLK("rk29_i2s.0", "i2s", &clk_i2s0),
	CLK("rk29_i2s.1", "i2s_div", &clk_i2s1_div),
	CLK("rk29_i2s.1", "i2s_frac_div", &clk_i2s1_frac_div),
	CLK("rk29_i2s.1", "i2s", &clk_i2s1),
	CLK(NULL, "spdif_div", &clk_spdif_div),
	CLK(NULL, "spdif_frac_div", &clk_spdif_frac_div),
	CLK(NULL, "spdif", &clk_spdif),

	CLK1(spi_src),
	CLK("rk29xx_spim.0", "spi0", &clk_spi0),
	CLK("rk29xx_spim.1", "spi1", &clk_spi1),

	CLK1(saradc),
	CLK1(timer0),
	CLK1(timer1),
	CLK1(timer2),
	CLK1(timer3),

	CLK1(sdmmc_src),
	CLK("rk29_sdmmc.0", "sdmmc", &clk_sdmmc0),
	CLK("rk29_sdmmc.0", "sdmmc_ahb", &clk_sdmmc0_ahb),
	CLK("rk29_sdmmc.1", "sdmmc", &clk_sdmmc1),
	CLK("rk29_sdmmc.1", "sdmmc_ahb", &clk_sdmmc1_ahb),
	CLK1(emmc),
	CLK1(emmc_ahb),
	CLK1(ddr),

	CLK1(uart01_src),
	CLK("rk29_serial.0", "uart", &clk_uart0),
	CLK("rk29_serial.0", "uart_div", &clk_uart0_div),
	CLK("rk29_serial.0", "uart_frac_div", &clk_uart0_frac_div),
	CLK("rk29_serial.1", "uart", &clk_uart1),
	CLK("rk29_serial.1", "uart_div", &clk_uart1_div),
	CLK("rk29_serial.1", "uart_frac_div", &clk_uart1_frac_div),

	CLK1(uart23_src),
	CLK("rk29_serial.2", "uart", &clk_uart2),
	CLK("rk29_serial.2", "uart_div", &clk_uart2_div),
	CLK("rk29_serial.2", "uart_frac_div", &clk_uart2_frac_div),
	CLK("rk29_serial.3", "uart", &clk_uart3),
	CLK("rk29_serial.3", "uart_div", &clk_uart3_div),
	CLK("rk29_serial.3", "uart_frac_div", &clk_uart3_frac_div),

	CLK1(hsadc_div),
	CLK1(hsadc_frac_div),
	CLK1(demod),
	CLK1(hsadc),
	CLK1(hsadc_div2),
	CLK1(hsadc_div2_inv),
	CLK1(hsadc_out),

	CLK(NULL, "dclk_lcdc_div", &dclk_lcdc_div),
	CLK(NULL, "dclk_lcdc", &dclk_lcdc),
	CLK(NULL, "dclk_ebook", &dclk_ebook),
	CLK(NULL, "aclk_lcdc", &aclk_lcdc),
	CLK(NULL, "hclk_lcdc", &hclk_lcdc),

	CLK(NULL, "aclk_vepu", &aclk_vepu),
	CLK(NULL, "hclk_vepu", &hclk_vepu),
	CLK(NULL, "aclk_vdpu", &aclk_vdpu),
	CLK(NULL, "hclk_vdpu", &hclk_vdpu),
	CLK1(gpu),
	CLK(NULL, "aclk_gpu", &aclk_gpu),

	CLK("rk29_i2c.0", "i2c", &clk_i2c0),
	CLK("rk29_i2c.1", "i2c", &clk_i2c1),
	CLK("rk29_i2c.2", "i2c", &clk_i2c2),
	CLK("rk29_i2c.3", "i2c", &clk_i2c3),

	CLK1(gpio0),
	CLK1(gpio1),
	CLK1(gpio2),
	CLK1(gpio3),
	CLK1(gpio4),
	CLK1(gpio5),
	CLK1(gpio6),
};

static LIST_HEAD(clocks);
static DEFINE_MUTEX(clocks_mutex);
static DEFINE_SPINLOCK(clockfw_lock);
#define LOCK() do { WARN_ON(in_irq()); if (!irqs_disabled()) spin_lock_bh(&clockfw_lock); } while (0)
#define UNLOCK() do { if (!irqs_disabled()) spin_unlock_bh(&clockfw_lock); } while (0)

static int clk_enable_nolock(struct clk *clk)
{
	int ret = 0;

	if (clk->usecount == 0) {
		if (clk->parent) {
			ret = clk_enable_nolock(clk->parent);
			if (ret)
				return ret;
		}

		if (clk->mode) {
			ret = clk->mode(clk, 1);
			if (ret) {
				if (clk->parent)
					clk_disable_nolock(clk->parent);
				return ret;
			}
		}
		pr_debug("%s enabled\n", clk->name);
	}
	clk->usecount++;

	return ret;
}

int clk_enable(struct clk *clk)
{
	int ret = 0;

	if (clk == NULL || IS_ERR(clk))
		return -EINVAL;

	LOCK();
	ret = clk_enable_nolock(clk);
	UNLOCK();

	return ret;
}
EXPORT_SYMBOL(clk_enable);

static void clk_disable_nolock(struct clk *clk)
{
	if (clk->usecount == 0) {
		printk(KERN_ERR "Trying disable clock %s with 0 usecount\n", clk->name);
		WARN_ON(1);
		return;
	}

	if (--clk->usecount == 0) {
		if (clk->mode)
			clk->mode(clk, 0);
		pr_debug("%s disabled\n", clk->name);
		if (clk->parent)
			clk_disable_nolock(clk->parent);
	}
}

void clk_disable(struct clk *clk)
{
	if (clk == NULL || IS_ERR(clk))
		return;

	LOCK();
	clk_disable_nolock(clk);
	UNLOCK();
}
EXPORT_SYMBOL(clk_disable);

unsigned long clk_get_rate(struct clk *clk)
{
	if (clk == NULL || IS_ERR(clk))
		return 0;

	return clk->rate;
}
EXPORT_SYMBOL(clk_get_rate);

/*-------------------------------------------------------------------------
 * Optional clock functions defined in include/linux/clk.h
 *-------------------------------------------------------------------------*/

/* Given a clock and a rate apply a clock specific rounding function */
static long clk_round_rate_nolock(struct clk *clk, unsigned long rate)
{
	if (clk->round_rate)
		return clk->round_rate(clk, rate);

	if (clk->flags & RATE_FIXED)
		printk(KERN_ERR "clock: clk_round_rate called on fixed-rate clock %s\n", clk->name);

	return clk->rate;
}

long clk_round_rate(struct clk *clk, unsigned long rate)
{
	long ret = 0;

	if (clk == NULL || IS_ERR(clk))
		return ret;

	LOCK();
	ret = clk_round_rate_nolock(clk, rate);
	UNLOCK();

	return ret;
}
EXPORT_SYMBOL(clk_round_rate);

static void __clk_recalc(struct clk *clk)
{
	if (unlikely(clk->flags & RATE_FIXED))
		return;
	if (clk->recalc)
		clk->rate = clk->recalc(clk);
	else if (clk->parent)
		clk->rate = clk->parent->rate;
	pr_debug("%s new clock rate is %lu\n", clk->name, clk->rate);
}

static int clk_set_rate_nolock(struct clk *clk, unsigned long rate)
{
	int ret;

	if (rate == clk->rate)
		return 0;

	pr_debug("set_rate for clock %s to rate %ld\n", clk->name, rate);

	if (clk->flags & CONFIG_PARTICIPANT)
		return -EINVAL;

	if (!clk->set_rate)
		return -EINVAL;

	ret = clk->set_rate(clk, rate);

	if (ret == 0) {
		__clk_recalc(clk);
		__propagate_rate(clk);
	}

	return ret;
}

/* Set the clock rate for a clock source */
int clk_set_rate(struct clk *clk, unsigned long rate)
{
	int ret = -EINVAL;

	if (clk == NULL || IS_ERR(clk))
		return ret;

	LOCK();
	ret = clk_set_rate_nolock(clk, rate);
	UNLOCK();

	return ret;
}
EXPORT_SYMBOL(clk_set_rate);

static int clk_set_parent_nolock(struct clk *clk, struct clk *parent)
{
	int ret;
	int enabled = clk->usecount > 0;
	struct clk *old_parent = clk->parent;

	if (clk->parent == parent)
		return 0;

	/* if clk is already enabled, enable new parent first and disable old parent later. */
	if (enabled)
		clk_enable_nolock(parent);

	if (clk->set_parent)
		ret = clk->set_parent(clk, parent);
	else
		ret = clksel_set_parent(clk, parent);

	if (ret == 0) {
		/* OK */
		__clk_reparent(clk, parent);
		__clk_recalc(clk);
		__propagate_rate(clk);
		if (enabled)
			clk_disable_nolock(old_parent);
	} else {
		if (enabled)
			clk_disable_nolock(parent);
	}

	return ret;
}

int clk_set_parent(struct clk *clk, struct clk *parent)
{
	int ret = -EINVAL;

	if (clk == NULL || IS_ERR(clk) || parent == NULL || IS_ERR(parent))
		return ret;

	if (clk->set_parent == NULL && clk->parents == NULL)
		return ret;

	LOCK();
	if (clk->usecount == 0)
		ret = clk_set_parent_nolock(clk, parent);
	else
		ret = -EBUSY;
	UNLOCK();

	return ret;
}
EXPORT_SYMBOL(clk_set_parent);

struct clk *clk_get_parent(struct clk *clk)
{
	return clk->parent;
}
EXPORT_SYMBOL(clk_get_parent);

static void __clk_reparent(struct clk *child, struct clk *parent)
{
	if (child->parent == parent)
		return;
	pr_debug("%s reparent to %s (was %s)\n", child->name, parent->name, ((child->parent) ? child->parent->name : "NULL"));

	list_del_init(&child->sibling);
	if (parent)
		list_add(&child->sibling, &parent->children);
	child->parent = parent;
}

/* Propagate rate to children */
static void __propagate_rate(struct clk *tclk)
{
	struct clk *clkp;

	list_for_each_entry(clkp, &tclk->children, sibling) {
		__clk_recalc(clkp);
		__propagate_rate(clkp);
	}
}

static LIST_HEAD(root_clks);

/**
 * recalculate_root_clocks - recalculate and propagate all root clocks
 *
 * Recalculates all root clocks (clocks with no parent), which if the
 * clock's .recalc is set correctly, should also propagate their rates.
 * Called at init.
 */
static void clk_recalculate_root_clocks_nolock(void)
{
	struct clk *clkp;

	list_for_each_entry(clkp, &root_clks, sibling) {
		__clk_recalc(clkp);
		__propagate_rate(clkp);
	}
}

void clk_recalculate_root_clocks(void)
{
	LOCK();
	clk_recalculate_root_clocks_nolock();
	UNLOCK();
}


/**
 * clk_preinit - initialize any fields in the struct clk before clk init
 * @clk: struct clk * to initialize
 *
 * Initialize any struct clk fields needed before normal clk initialization
 * can run.  No return value.
 */
static void clk_preinit(struct clk *clk)
{
	INIT_LIST_HEAD(&clk->children);
}

static int clk_register(struct clk *clk)
{
	if (clk == NULL || IS_ERR(clk))
		return -EINVAL;

	/*
	 * trap out already registered clocks
	 */
	if (clk->node.next || clk->node.prev)
		return 0;

	mutex_lock(&clocks_mutex);

	if (clk->get_parent)
		clk->parent = clk->get_parent(clk);
	else if (clk->parents)
		clk->parent = clksel_get_parent(clk);

	if (clk->parent)
		list_add(&clk->sibling, &clk->parent->children);
	else
		list_add(&clk->sibling, &root_clks);

	list_add(&clk->node, &clocks);

	mutex_unlock(&clocks_mutex);

	return 0;
}

static void clk_enable_init_clocks(void)
{
	struct clk *clkp;

	list_for_each_entry(clkp, &clocks, node) {
		if (clkp->flags & ENABLE_ON_INIT)
			clk_enable(clkp);
	}
}

static void rk29_clock_common_init(void)
{
	/* periph pll */
	clk_set_rate_nolock(&periph_pll_clk, 624 * MHZ);
	clk_set_parent_nolock(&aclk_periph, &periph_pll_clk);	// default
	clk_set_rate_nolock(&aclk_periph, 312 * MHZ);	// default
	clk_set_rate_nolock(&hclk_periph, 156 * MHZ);	// default
	clk_set_rate_nolock(&pclk_periph, 78 * MHZ);	// default
	clk_set_parent_nolock(&clk_uhost, &periph_pll_clk);	// default
	clk_set_rate_nolock(&clk_uhost, 48 * MHZ);
	clk_set_parent_nolock(&clk_i2s0_div, &periph_pll_clk);	// default
	clk_set_parent_nolock(&clk_i2s1_div, &periph_pll_clk);	// default
	clk_set_parent_nolock(&clk_spdif_div, &periph_pll_clk);	// default
	clk_set_parent_nolock(&clk_spi_src, &periph_pll_clk);	// default
	clk_set_parent_nolock(&clk_sdmmc_src, &periph_pll_clk);
	clk_set_parent_nolock(&clk_uart01_src, &periph_pll_clk);	// default
	clk_set_parent_nolock(&clk_uart23_src, &periph_pll_clk);	// default
	clk_set_parent_nolock(&dclk_lcdc_div, &periph_pll_clk);
	clk_set_parent_nolock(&aclk_lcdc, &periph_pll_clk);
	clk_set_parent_nolock(&aclk_vepu, &periph_pll_clk);	// default
	clk_set_parent_nolock(&aclk_vdpu, &periph_pll_clk);	// default
	clk_set_parent_nolock(&clk_gpu, &periph_pll_clk);	// default
	clk_set_parent_nolock(&aclk_gpu, &periph_pll_clk);	// default
	clk_set_parent_nolock(&clk_mac_ref_div, &periph_pll_clk);
	clk_set_parent_nolock(&clk_hsadc_div, &periph_pll_clk);

	/* arm pll */
	clk_set_rate_nolock(&arm_pll_clk, 600 * MHZ);
}

void __init rk29_clock_init(void)
{
	struct clk_lookup *lk;

	for (lk = clks; lk < clks + ARRAY_SIZE(clks); lk++)
		clk_preinit(lk->clk);

	for (lk = clks; lk < clks + ARRAY_SIZE(clks); lk++) {
		clkdev_add(lk);
		clk_register(lk->clk);
	}

	clk_recalculate_root_clocks_nolock();

	rk29_clock_common_init();

	printk(KERN_INFO "Clocking rate (apll/dpll/cpll/ppll/core/aclk/hclk/pclk): %ld/%ld/%ld/%ld/%ld/%ld/%ld/%ld MHz\n",
	       arm_pll_clk.rate / MHZ, ddr_pll_clk.rate / MHZ, codec_pll_clk.rate / MHZ, periph_pll_clk.rate / MHZ,
	       clk_core.rate / MHZ, aclk_cpu.rate / MHZ, hclk_cpu.rate / MHZ, pclk_cpu.rate / MHZ);

	/*
	 * Only enable those clocks we will need, let the drivers
	 * enable other clocks as necessary
	 */
	clk_enable_init_clocks();
}

#ifdef CONFIG_PROC_FS
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

static void dump_clock(struct seq_file *s, struct clk *clk, int deep)
{
	struct clk* ck;
	int i;
	unsigned long rate = clk->rate;

	for (i = 0; i < deep; i++)
		seq_printf(s, "    ");

	seq_printf(s, "%-9s ", clk->name);

	if (clk->mode && (clk->gate_idx < CLK_GATE_MAX)) {
		u32 reg;
		int idx = clk->gate_idx;
		u32 v;

		reg = CRU_CLKGATE0_CON;
		reg += (idx >> 5) << 2;
		idx &= 0x1F;

		v = cru_readl(reg) & (1 << idx);
		
		seq_printf(s, "%s ", v ? "off" : "on ");
	}

	if (rate >= MHZ) {
		if (rate % MHZ)
			seq_printf(s, "%ld.%06ld MHz", rate / MHZ, rate % MHZ);
		else
			seq_printf(s, "%ld MHz", rate / MHZ);
	} else if (rate >= KHZ) {
		if (rate % KHZ)
			seq_printf(s, "%ld.%03ld KHz", rate / KHZ, rate % KHZ);
		else
			seq_printf(s, "%ld KHz", rate / KHZ);
	} else {
		seq_printf(s, "%ld Hz", rate);
	}

	seq_printf(s, " usecount = %d", clk->usecount);

	seq_printf(s, " parent = %s\n", clk->parent ? clk->parent->name : "NULL");

	list_for_each_entry(ck, &clocks, node) {
		if (ck->parent == clk)
			dump_clock(s, ck, deep + 1);
	}
}

static int proc_clk_show(struct seq_file *s, void *v)
{
	struct clk* clk;

	mutex_lock(&clocks_mutex);
	list_for_each_entry(clk, &clocks, node) {
		if (!clk->parent)
			dump_clock(s, clk, 0);
	}
	mutex_unlock(&clocks_mutex);

	seq_printf(s, "\nRegisters:\n");
	seq_printf(s, "APLL     : 0x%08x\n", cru_readl(CRU_APLL_CON));
	seq_printf(s, "DPLL     : 0x%08x\n", cru_readl(CRU_DPLL_CON));
	seq_printf(s, "CPLL     : 0x%08x\n", cru_readl(CRU_CPLL_CON));
	seq_printf(s, "PPLL     : 0x%08x\n", cru_readl(CRU_PPLL_CON));
	seq_printf(s, "MODE     : 0x%08x\n", cru_readl(CRU_MODE_CON));
	seq_printf(s, "CLKSEL0  : 0x%08x\n", cru_readl(CRU_CLKSEL0_CON));
	seq_printf(s, "CLKSEL1  : 0x%08x\n", cru_readl(CRU_CLKSEL1_CON));
	seq_printf(s, "CLKSEL2  : 0x%08x\n", cru_readl(CRU_CLKSEL2_CON));
	seq_printf(s, "CLKSEL3  : 0x%08x\n", cru_readl(CRU_CLKSEL3_CON));
	seq_printf(s, "CLKSEL4  : 0x%08x\n", cru_readl(CRU_CLKSEL4_CON));
	seq_printf(s, "CLKSEL5  : 0x%08x\n", cru_readl(CRU_CLKSEL5_CON));
	seq_printf(s, "CLKSEL6  : 0x%08x\n", cru_readl(CRU_CLKSEL6_CON));
	seq_printf(s, "CLKSEL7  : 0x%08x\n", cru_readl(CRU_CLKSEL7_CON));
	seq_printf(s, "CLKSEL8  : 0x%08x\n", cru_readl(CRU_CLKSEL8_CON));
	seq_printf(s, "CLKSEL9  : 0x%08x\n", cru_readl(CRU_CLKSEL9_CON));
	seq_printf(s, "CLKSEL10 : 0x%08x\n", cru_readl(CRU_CLKSEL10_CON));
	seq_printf(s, "CLKSEL11 : 0x%08x\n", cru_readl(CRU_CLKSEL11_CON));
	seq_printf(s, "CLKSEL12 : 0x%08x\n", cru_readl(CRU_CLKSEL12_CON));
	seq_printf(s, "CLKSEL13 : 0x%08x\n", cru_readl(CRU_CLKSEL13_CON));
	seq_printf(s, "CLKSEL14 : 0x%08x\n", cru_readl(CRU_CLKSEL14_CON));
	seq_printf(s, "CLKSEL15 : 0x%08x\n", cru_readl(CRU_CLKSEL15_CON));
	seq_printf(s, "CLKSEL16 : 0x%08x\n", cru_readl(CRU_CLKSEL16_CON));
	seq_printf(s, "CLKSEL17 : 0x%08x\n", cru_readl(CRU_CLKSEL17_CON));
	seq_printf(s, "CLKGATE0 : 0x%08x\n", cru_readl(CRU_CLKGATE0_CON));
	seq_printf(s, "CLKGATE1 : 0x%08x\n", cru_readl(CRU_CLKGATE1_CON));
	seq_printf(s, "CLKGATE2 : 0x%08x\n", cru_readl(CRU_CLKGATE2_CON));
	seq_printf(s, "CLKGATE3 : 0x%08x\n", cru_readl(CRU_CLKGATE3_CON));

	return 0;
}

static int proc_clk_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_clk_show, NULL);
}

static const struct file_operations proc_clk_fops = {
	.open		= proc_clk_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};

static int __init clk_proc_init(void)
{
	proc_create("clocks", 0, NULL, &proc_clk_fops);
	return 0;

}
late_initcall(clk_proc_init);
#endif /* CONFIG_PROC_FS */
