// SPDX-License-Identifier: GPL-2.0-only
/*
 * JDI JD35695 1080x1920 command mode DSI panel (Novatek NT35695 driver IC),
 * as fitted to the OnePlus 2.  The init sequence and timings are the
 * OnePlus 2 downstream device tree's (qcom,mdss_dsi_jd35695_1080p_cmd).
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct jd35695 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *vddio;
	struct gpio_desc *reset_gpio;
};

static inline struct jd35695 *to_jd35695(struct drm_panel *panel)
{
	return container_of(panel, struct jd35695, panel);
}

static void jd35695_reset(struct jd35695 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(11000, 12000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(2000, 3000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(12000, 13000);
}

static int jd35695_on(struct jd35695 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_set_column_address_multi(&dsi_ctx, 0x0000, 0x0437);
	mipi_dsi_dcs_set_page_address_multi(&dsi_ctx, 0x0000, 0x077f);
	mipi_dsi_dcs_set_tear_scanline_multi(&dsi_ctx, 0x0500);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x86);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb6, 0x77);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb8, 0xad);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_ADDRESS_MODE, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x08, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x2c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x05, 0x24);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x01, 0x84);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 105);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 45);

	return dsi_ctx.accum_err;
}

static int jd35695_off(struct jd35695 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x00);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 100);

	return dsi_ctx.accum_err;
}

static int jd35695_prepare(struct drm_panel *panel)
{
	struct jd35695 *ctx = to_jd35695(panel);
	int ret;

	ret = regulator_enable(ctx->vddio);
	if (ret)
		return ret;

	jd35695_reset(ctx);

	ret = jd35695_on(ctx);
	if (ret < 0) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_disable(ctx->vddio);
		return ret;
	}

	return 0;
}

static int jd35695_unprepare(struct drm_panel *panel)
{
	struct jd35695 *ctx = to_jd35695(panel);

	jd35695_off(ctx);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_disable(ctx->vddio);

	return 0;
}

/* h: front porch 100, sync 8, back porch 82; v: 3, 2, 7; 60 Hz */
static const struct drm_display_mode jd35695_mode = {
	.clock = (1080 + 100 + 8 + 82) * (1920 + 3 + 2 + 7) * 60 / 1000,
	.hdisplay = 1080,
	.hsync_start = 1080 + 100,
	.hsync_end = 1080 + 100 + 8,
	.htotal = 1080 + 100 + 8 + 82,
	.vdisplay = 1920,
	.vsync_start = 1920 + 3,
	.vsync_end = 1920 + 3 + 2,
	.vtotal = 1920 + 3 + 2 + 7,
	.width_mm = 70,
	.height_mm = 127,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int jd35695_get_modes(struct drm_panel *panel,
			     struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &jd35695_mode);
}

static const struct drm_panel_funcs jd35695_panel_funcs = {
	.prepare = jd35695_prepare,
	.unprepare = jd35695_unprepare,
	.get_modes = jd35695_get_modes,
};

static int jd35695_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct jd35695 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct jd35695, panel,
				   &jd35695_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->vddio = devm_regulator_get(dev, "vddio");
	if (IS_ERR(ctx->vddio))
		return dev_err_probe(dev, PTR_ERR(ctx->vddio),
				     "failed to get vddio regulator\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void jd35695_remove(struct mipi_dsi_device *dsi)
{
	struct jd35695 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id jd35695_of_match[] = {
	{ .compatible = "jdi,jd35695" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, jd35695_of_match);

static struct mipi_dsi_driver jd35695_driver = {
	.probe = jd35695_probe,
	.remove = jd35695_remove,
	.driver = {
		.name = "panel-jdi-jd35695",
		.of_match_table = jd35695_of_match,
	},
};
module_mipi_dsi_driver(jd35695_driver);

MODULE_DESCRIPTION("DRM driver for the JDI JD35695 1080p command mode panel");
MODULE_LICENSE("GPL");
