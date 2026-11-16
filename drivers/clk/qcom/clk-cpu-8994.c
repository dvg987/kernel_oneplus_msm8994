// SPDX-License-Identifier: GPL-2.0-only
/*
 * MSM8994 CPU cluster clocks
 *
 * Each cluster has two variable rate PLLs (VCO 1209.6 - 1996.8 MHz) and two
 * muxes in its APCS global block.  The low frequency mux takes either PLL's
 * main output through its post-divider (2 or 4), the high frequency mux
 * either PLL's early output undivided, or the low frequency mux.
 *
 * A PLL cannot be relocked while the cluster runs from it, and parking the
 * cluster on a fixed source while it relocks would need a rate that is safe
 * at whatever voltage the cluster has at the time.  So, as the vendor driver
 * does, a rate change programs and locks the PLL that is not in use and
 * moves the muxes over to it; the old PLL is then switched off.  Each
 * cluster is therefore a single clock here, with the board's XO as parent.
 *
 * Copyright (c) 2014-2015, The Linux Foundation. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk-provider.h>
#include <linux/cpumask.h>
#include <linux/delay.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/regmap.h>
#include <linux/smp.h>

#define PLL_VCO_MIN		1209600000UL
#define PLL_VCO_MAX		1996800000UL

/* Each cluster's PLL1 sits 0x40 above its PLL0 */
#define PLL_OFFSET(n)		((n) * 0x40)
#define PLL_MODE(n)		(PLL_OFFSET(n) + 0x00)
#define  PLL_OUTCTRL		BIT(0)
#define  PLL_BYPASSNL		BIT(1)
#define  PLL_RESET_N		BIT(2)
#define  PLL_APC_PDN		BIT(24)
#define  PLL_LOCK_DET		BIT(31)
#define PLL_L_VAL(n)		(PLL_OFFSET(n) + 0x04)
#define  PLL_L_VAL_MASK		GENMASK(9, 0)
#define PLL_ALPHA(n)		(PLL_OFFSET(n) + 0x08)
#define PLL_USER_CTL(n)		(PLL_OFFSET(n) + 0x10)
#define  PLL_MAIN_OUT		BIT(0)
#define  PLL_EARLY_OUT		BIT(3)
#define  PLL_POST_DIV		GENMASK(9, 8)
#define  PLL_POST_DIV_2		1
#define  PLL_POST_DIV_4		3
#define  PLL_PRE_DIV		BIT(12)
#define  PLL_MN_EN		BIT(24)
#define PLL_CONFIG_CTL(n)	(PLL_OFFSET(n) + 0x14)
#define PLL_TEST_CTL_LO(n)	(PLL_OFFSET(n) + 0x20)
#define PLL_TEST_CTL_HI(n)	(PLL_OFFSET(n) + 0x24)

/* MSM8994 v2 settings */
#define PLL_CONFIG_CTL_VAL	0x004d6968
#define PLL_TEST_CTL_LO_VAL	0x80000000
#define PLL_TEST_CTL_HI_VAL	0x00000001

/* In the cluster's APCS global block */
#define MUX_REG			0x54
#define  MUX_LF			GENMASK(2, 1)
#define  MUX_LF_PLL1_DIV	1
#define  MUX_LF_PLL0_DIV	2
#define  MUX_HF			GENMASK(4, 3)
#define  MUX_HF_LF		0
#define  MUX_HF_PLL1		1
#define  MUX_HF_PLL0		3

struct cpu_8994_cluster {
	struct clk_hw hw;
	struct regmap *pll;
	struct regmap *glb;
	u32 user_ctl;		/* of the PLL in use at boot */
	struct cpumask cpus;
	struct pm_qos_request qos;
};

#define to_cluster(_hw) container_of(_hw, struct cpu_8994_cluster, hw)

static unsigned int cpu_8994_post_div(u32 user_ctl)
{
	switch (FIELD_GET(PLL_POST_DIV, user_ctl)) {
	case PLL_POST_DIV_2:
		return 2;
	case PLL_POST_DIV_4:
		return 4;
	default:
		return 1;
	}
}

/* The PLL the cluster runs from and its divider, or -ENODEV for neither */
static int cpu_8994_cur_pll(struct cpu_8994_cluster *c, unsigned int *div)
{
	u32 mux, user;
	int n;

	regmap_read(c->glb, MUX_REG, &mux);

	switch (FIELD_GET(MUX_HF, mux)) {
	case MUX_HF_PLL0:
		*div = 1;
		return 0;
	case MUX_HF_PLL1:
		*div = 1;
		return 1;
	case MUX_HF_LF:
		break;
	default:
		return -ENODEV;
	}

	switch (FIELD_GET(MUX_LF, mux)) {
	case MUX_LF_PLL0_DIV:
		n = 0;
		break;
	case MUX_LF_PLL1_DIV:
		n = 1;
		break;
	default:
		return -ENODEV;
	}

	regmap_read(c->pll, PLL_USER_CTL(n), &user);
	*div = cpu_8994_post_div(user);
	return n;
}

static unsigned long cpu_8994_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct cpu_8994_cluster *c = to_cluster(hw);
	unsigned int div;
	u32 l;
	int n;

	n = cpu_8994_cur_pll(c, &div);
	if (n < 0)
		return parent_rate;

	regmap_read(c->pll, PLL_L_VAL(n), &l);
	return (l & PLL_L_VAL_MASK) * parent_rate / div;
}

/* The L value and post-divider for a rate, and the rate they give */
static unsigned long cpu_8994_config(unsigned long rate, unsigned long xo,
				     u32 *l, unsigned int *div)
{
	unsigned int d;

	for (d = 1; d <= 4; d *= 2) {
		unsigned long vco = rate * d;

		if (vco + xo / 2 < PLL_VCO_MIN || vco > PLL_VCO_MAX)
			continue;

		*l = DIV_ROUND_CLOSEST(vco, xo);
		*div = d;
		return *l * xo / d;
	}

	return 0;
}

static int cpu_8994_determine_rate(struct clk_hw *hw,
				   struct clk_rate_request *req)
{
	unsigned long rate;
	unsigned int div;
	u32 l;

	rate = cpu_8994_config(req->rate, req->best_parent_rate, &l, &div);
	if (!rate)
		return -EINVAL;

	req->rate = rate;
	return 0;
}

static void cpu_8994_pll_disable(struct cpu_8994_cluster *c, unsigned int n)
{
	regmap_clear_bits(c->pll, PLL_MODE(n), PLL_OUTCTRL);
	udelay(1);
	regmap_clear_bits(c->pll, PLL_MODE(n), PLL_RESET_N | PLL_BYPASSNL);
}

static bool cpu_8994_pll_locked(struct cpu_8994_cluster *c, unsigned int n)
{
	u32 mode;

	regmap_read(c->pll, PLL_MODE(n), &mode);
	return mode & PLL_LOCK_DET;
}

static int cpu_8994_pll_enable(struct cpu_8994_cluster *c, unsigned int n,
			       u32 l, unsigned int div)
{
	u32 user = c->user_ctl, mode;
	int ret;

	cpu_8994_pll_disable(c, n);

	user &= ~(PLL_POST_DIV | PLL_PRE_DIV | PLL_MN_EN);
	user |= PLL_MAIN_OUT | PLL_EARLY_OUT;
	user |= FIELD_PREP(PLL_POST_DIV,
			   div == 4 ? PLL_POST_DIV_4 : PLL_POST_DIV_2);
	regmap_write(c->pll, PLL_USER_CTL(n), user);

	regmap_clear_bits(c->pll, PLL_MODE(n), PLL_APC_PDN);
	regmap_write(c->pll, PLL_ALPHA(n), 0);
	regmap_write(c->pll, PLL_CONFIG_CTL(n), PLL_CONFIG_CTL_VAL);
	regmap_write(c->pll, PLL_TEST_CTL_LO(n), PLL_TEST_CTL_LO_VAL);
	regmap_write(c->pll, PLL_TEST_CTL_HI(n), PLL_TEST_CTL_HI_VAL);
	regmap_write(c->pll, PLL_L_VAL(n), l);

	/* At least 5 us between leaving bypass and leaving reset */
	regmap_set_bits(c->pll, PLL_MODE(n), PLL_BYPASSNL);
	usleep_range(10, 20);
	regmap_set_bits(c->pll, PLL_MODE(n), PLL_RESET_N);

	/*
	 * The lock detector can report a transient lock about 16 us out of
	 * reset: wait past it, and check the lock twice, as the vendor
	 * driver does.
	 */
	fsleep(200);
	ret = regmap_read_poll_timeout(c->pll, PLL_MODE(n), mode,
				       mode & PLL_LOCK_DET, 1, 2000);
	if (!ret) {
		udelay(1);
		if (!cpu_8994_pll_locked(c, n))
			ret = -ETIMEDOUT;
	}
	if (ret) {
		pr_err("%s: PLL%u did not lock for L=%u\n",
		       clk_hw_get_name(&c->hw), n, l);
		cpu_8994_pll_disable(c, n);
		return ret;
	}

	regmap_set_bits(c->pll, PLL_MODE(n), PLL_OUTCTRL);
	udelay(1);

	return 0;
}

static int cpu_8994_switch(struct cpu_8994_cluster *c, u32 l,
			   unsigned int div)
{
	unsigned int old_div;
	int old, new, ret;
	u32 mux;

	old = cpu_8994_cur_pll(c, &old_div);
	new = old == 0 ? 1 : 0;

	ret = cpu_8994_pll_enable(c, new, l, div);
	if (ret)
		return ret;

	if (div == 1) {
		regmap_update_bits(c->glb, MUX_REG, MUX_HF,
				   FIELD_PREP(MUX_HF, new ? MUX_HF_PLL1 :
							    MUX_HF_PLL0));
	} else {
		/*
		 * The low frequency mux first: while the high frequency mux
		 * is on a PLL nothing runs from it, and while it is on the
		 * low frequency mux this is the switch itself.
		 */
		regmap_update_bits(c->glb, MUX_REG, MUX_LF,
				   FIELD_PREP(MUX_LF, new ? MUX_LF_PLL1_DIV :
							    MUX_LF_PLL0_DIV));
		regmap_read(c->glb, MUX_REG, &mux);
		if (FIELD_GET(MUX_HF, mux) != MUX_HF_LF) {
			udelay(1);
			regmap_update_bits(c->glb, MUX_REG, MUX_HF,
					   FIELD_PREP(MUX_HF, MUX_HF_LF));
		}
	}
	udelay(5);

	if (old >= 0)
		cpu_8994_pll_disable(c, old);


	return 0;
}

static void cpu_8994_wake(void *unused)
{
}

static int cpu_8994_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct cpu_8994_cluster *c = to_cluster(hw);
	unsigned int div;
	int ret;
	u32 l;

	if (cpu_8994_config(rate, parent_rate, &l, &div) != rate)
		return -EINVAL;

	/* Keep the cluster's cores out of power collapse across the switch */
	cpu_latency_qos_add_request(&c->qos, 0);
	on_each_cpu_mask(&c->cpus, cpu_8994_wake, NULL, true);

	ret = cpu_8994_switch(c, l, div);

	cpu_latency_qos_remove_request(&c->qos);

	return ret;
}

static const struct clk_ops cpu_8994_ops = {
	.recalc_rate = cpu_8994_recalc_rate,
	.determine_rate = cpu_8994_determine_rate,
	.set_rate = cpu_8994_set_rate,
};

static const struct regmap_config cpu_8994_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = 0xffc,
	.fast_io = true,
};

static const char * const cpu_8994_names[] = { "a53_clk", "a57_clk" };

/* The CPUs whose clock is the cluster clock with index @idx */
static void cpu_8994_get_cpus(struct device *dev, unsigned int idx,
			      struct cpumask *cpus)
{
	struct of_phandle_args args;
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct device_node *np = of_cpu_device_node_get(cpu);

		if (!np)
			continue;

		if (!of_parse_phandle_with_args(np, "clocks", "#clock-cells",
						0, &args)) {
			if (args.np == dev->of_node && args.args_count == 1 &&
			    args.args[0] == idx)
				cpumask_set_cpu(cpu, cpus);
			of_node_put(args.np);
		}
		of_node_put(np);
	}
}

static int cpu_8994_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct clk_parent_data xo = { .fw_name = "xo" };
	struct clk_hw_onecell_data *data;
	unsigned int i;
	int ret;

	data = devm_kzalloc(dev, struct_size(data, hws,
					     ARRAY_SIZE(cpu_8994_names)),
			    GFP_KERNEL);
	if (!data)
		return -ENOMEM;
	data->num = ARRAY_SIZE(cpu_8994_names);

	for (i = 0; i < ARRAY_SIZE(cpu_8994_names); i++) {
		struct clk_init_data init = {
			.name = cpu_8994_names[i],
			.parent_data = &xo,
			.num_parents = 1,
			.ops = &cpu_8994_ops,
			.flags = CLK_IS_CRITICAL,
		};
		struct cpu_8994_cluster *c;
		void __iomem *base;
		unsigned int div;
		int n;

		c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
		if (!c)
			return -ENOMEM;

		base = devm_platform_ioremap_resource(pdev, i);
		if (IS_ERR(base))
			return PTR_ERR(base);
		c->pll = devm_regmap_init_mmio(dev, base,
					       &cpu_8994_regmap_config);
		if (IS_ERR(c->pll))
			return PTR_ERR(c->pll);

		/*
		 * Cluster 0's muxes are in the APCS global block that also
		 * holds the IPC mailbox; cluster 1's block is ours alone.
		 */
		if (i == 0) {
			c->glb = syscon_regmap_lookup_by_phandle(dev->of_node,
								 "qcom,apcs");
		} else {
			base = devm_platform_ioremap_resource(pdev, 2);
			if (IS_ERR(base))
				return PTR_ERR(base);
			c->glb = devm_regmap_init_mmio(dev, base,
						       &cpu_8994_regmap_config);
		}
		if (IS_ERR(c->glb))
			return PTR_ERR(c->glb);

		n = cpu_8994_cur_pll(c, &div);
		if (n < 0)
			return dev_err_probe(dev, n, "%s is not on a PLL\n",
					     cpu_8994_names[i]);
		regmap_read(c->pll, PLL_USER_CTL(n), &c->user_ctl);
		cpu_8994_get_cpus(dev, i, &c->cpus);

		c->hw.init = &init;
		ret = devm_clk_hw_register(dev, &c->hw);
		if (ret)
			return ret;
		data->hws[i] = &c->hw;

	}

	return devm_of_clk_add_hw_provider(dev, of_clk_hw_onecell_get, data);
}

static const struct of_device_id cpu_8994_match[] = {
	{ .compatible = "qcom,msm8994-cpucc" },
	{ }
};
MODULE_DEVICE_TABLE(of, cpu_8994_match);

static struct platform_driver cpu_8994_driver = {
	.probe = cpu_8994_probe,
	.driver = {
		.name = "qcom-msm8994-cpucc",
		.of_match_table = cpu_8994_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(cpu_8994_driver);

MODULE_DESCRIPTION("Qualcomm MSM8994 CPU clock driver");
MODULE_LICENSE("GPL");
