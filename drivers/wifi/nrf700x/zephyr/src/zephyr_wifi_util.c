/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/* @file
 * @brief NRF Wi-Fi util shell module
 */
#include <stdlib.h>
#include "host_rpu_umac_if.h"
#include "fmac_api.h"
#include "zephyr_util.h"
#include "zephyr_fmac_main.h"
#include "zephyr_wifi_util.h"

extern struct wifi_nrf_drv_priv_zep rpu_drv_priv_zep;
struct wifi_nrf_ctx_zep *ctx = &rpu_drv_priv_zep.rpu_ctx_zep;


int nrf_wifi_util_conf_init(struct rpu_conf_params *conf_params)
{
	if (!conf_params) {
		return -ENOEXEC;
	}

	memset(conf_params, 0, sizeof(*conf_params));

	/* Initialize values which are other than 0 */
	conf_params->he_ltf = -1;
	conf_params->he_gi = -1;
	return 0;
}


static int nrf_wifi_util_set_he_ltf(const struct shell *shell,
				    size_t argc,
				    const char *argv[])
{
	char *ptr = NULL;
	unsigned long he_ltf = 0;

	he_ltf = strtoul(argv[1], &ptr, 10);

	if (he_ltf > 2) {
		shell_fprintf(shell,
			      SHELL_ERROR,
			      "Invalid HE LTF value(%lu).\n",
			      he_ltf);
		shell_help(shell);
		return -ENOEXEC;
	}

	ctx->conf_params.he_ltf = he_ltf;

	return 0;
}


static int nrf_wifi_util_set_he_gi(const struct shell *shell,
				   size_t argc,
				   const char *argv[])
{
	char *ptr = NULL;
	unsigned long he_gi = 0;

	he_gi = strtoul(argv[1], &ptr, 10);

	if (he_gi > 2) {
		shell_fprintf(shell,
			      SHELL_ERROR,
			      "Invalid HE GI value(%lu).\n",
			      he_gi);
		shell_help(shell);
		return -ENOEXEC;
	}

	ctx->conf_params.he_gi = he_gi;

	return 0;
}


static int nrf_wifi_util_set_he_ltf_gi(const struct shell *shell,
				       size_t argc,
				       const char *argv[])
{
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	char *ptr = NULL;
	unsigned long val = 0;

	val = strtoul(argv[1], &ptr, 10);

	if ((val < 0) || (val > 1)) {
		shell_fprintf(shell,
			      SHELL_ERROR,
			      "Invalid value(%lu).\n",
			      val);
		shell_help(shell);
		return -ENOEXEC;
	}

	if (ctx->conf_params.set_he_ltf_gi != val) {
		status = wifi_nrf_fmac_conf_ltf_gi(ctx->rpu_ctx,
						   ctx->conf_params.he_ltf,
						   ctx->conf_params.he_gi,
						   val);

		if (status != WIFI_NRF_STATUS_SUCCESS) {
			shell_fprintf(shell,
				      SHELL_ERROR,
				      "Programming ltf_gi failed\n");
			return -ENOEXEC;
		}

		ctx->conf_params.set_he_ltf_gi = val;
	}

	return 0;
}


static int nrf_wifi_util_set_rts_threshold(const struct shell *shell,
					   size_t argc,
					   const char *argv[])
{
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	char *ptr = NULL;
	unsigned long val = 0;
	struct nrf_wifi_umac_set_wiphy_info wiphy_info;

	memset(&wiphy_info, 0, sizeof(struct nrf_wifi_umac_set_wiphy_info));

	val = strtoul(argv[1], &ptr, 10);

	if (ctx->conf_params.rts_threshold != val) {

		wiphy_info.rts_threshold = val;

		status = wifi_nrf_fmac_set_wiphy_params(ctx->rpu_ctx,
							0,
							&wiphy_info);

		if (status != WIFI_NRF_STATUS_SUCCESS) {
			shell_fprintf(shell,
				      SHELL_ERROR,
				      "Programming threshold failed\n");
			return -ENOEXEC;
		}

		ctx->conf_params.rts_threshold = val;
	}

	return 0;
}


static int nrf_wifi_util_set_uapsd_queue(const struct shell *shell,
					 size_t argc,
					 const char *argv[])
{
	enum wifi_nrf_status status = WIFI_NRF_STATUS_FAIL;
	char *ptr = NULL;
	unsigned long val = 0;

	val = strtoul(argv[1], &ptr, 10);

	if ((val < UAPSD_Q_MIN) || (val > UAPSD_Q_MAX)) {
		shell_fprintf(shell,
			      SHELL_ERROR,
			      "Invalid value(%lu).\n",
			      val);
		shell_help(shell);
		return -ENOEXEC;
	}

	if (ctx->conf_params.uapsd_queue != val) {
		status = wifi_nrf_fmac_set_uapsd_queue(ctx->rpu_ctx,
						       0,
						       val);

		if (status != WIFI_NRF_STATUS_SUCCESS) {
			shell_fprintf(shell,
				      SHELL_ERROR,
				      "Programming uapsd_queue failed\n");
			return -ENOEXEC;
		}

		ctx->conf_params.uapsd_queue = val;
	}

	return 0;
}


static int nrf_wifi_util_show_cfg(const struct shell *shell,
				  size_t argc,
				  const char *argv[])
{
	struct rpu_conf_params *conf_params = NULL;

	conf_params = &ctx->conf_params;

	shell_fprintf(shell,
		      SHELL_INFO,
		      "************* Configured Parameters ***********\n");
	shell_fprintf(shell,
		      SHELL_INFO,
		      "\n");

	shell_fprintf(shell,
		      SHELL_INFO,
		      "he_ltf = %d\n",
		      conf_params->he_ltf);

	shell_fprintf(shell,
		      SHELL_INFO,
		      "he_gi = %u\n",
		      conf_params->he_gi);

	shell_fprintf(shell,
		      SHELL_INFO,
		      "set_he_ltf_gi = %d\n",
		      conf_params->set_he_ltf_gi);

	shell_fprintf(shell,
		      SHELL_INFO,
		      "rts_threshold = %d\n",
		      conf_params->rts_threshold);

	shell_fprintf(shell,
		      SHELL_INFO,
		      "uapsd_queue = %d\n",
		      conf_params->uapsd_queue);
	return 0;
}


SHELL_STATIC_SUBCMD_SET_CREATE(
	nrf_wifi_util_subcmds,
	SHELL_CMD_ARG(he_ltf,
		      NULL,
		      "0 - 1x HE LTF\n"
		      "1 - 2x HE LTF\n"
		      "2 - 4x HE LTF                                        ",
		      nrf_wifi_util_set_he_ltf,
		      2,
		      0),
	SHELL_CMD_ARG(he_gi,
		      NULL,
		      "0 - 0.8 us\n"
		      "1 - 1.6 us\n"
		      "2 - 3.2 us                                           ",
		      nrf_wifi_util_set_he_gi,
		      2,
		      0),
	SHELL_CMD_ARG(set_he_ltf_gi,
		      NULL,
		      "0 - Disable\n"
		      "1 - Enable",
		      nrf_wifi_util_set_he_ltf_gi,
		      2,
		      0),
	SHELL_CMD_ARG(rts_threshold,
		      NULL,
		      "<val> - Value  > 0",
		      nrf_wifi_util_set_rts_threshold,
		      2,
		      0),
	SHELL_CMD_ARG(uapsd_queue,
		      NULL,
		      "<val> - 0 to 15",
		      nrf_wifi_util_set_uapsd_queue,
		      2,
		      0),
	SHELL_CMD_ARG(show_config,
		      NULL,
		      "Display the current configuration values",
		      nrf_wifi_util_show_cfg,
		      1,
		      0),
	SHELL_SUBCMD_SET_END);


SHELL_CMD_REGISTER(wifi_util,
		   &nrf_wifi_util_subcmds,
		   "nRF Wi-Fi utility shell commands",
		   NULL);


static int nrf_wifi_util_init(const struct device *unused)
{
	ARG_UNUSED(unused);

	if (nrf_wifi_util_conf_init(&ctx->conf_params) < 0)
		return -1;

	return 0;
}


SYS_INIT(nrf_wifi_util_init,
	 APPLICATION,
	 CONFIG_APPLICATION_INIT_PRIORITY);
