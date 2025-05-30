/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <stdio.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>
#include <modem/modem_info.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>
#include <app_event_manager.h>
#include <caf/events/button_event.h>
#define MODULE main
#include <caf/events/module_state_event.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_rest.h>

LOG_MODULE_REGISTER(nrf_cloud_rest_cell_location_sample,
		    CONFIG_NRF_CLOUD_REST_CELL_LOCATION_SAMPLE_LOG_LEVEL);

#define BTN_NUM			1 /* See include/buttons_def.h */
#define JITP_REQ_WAIT_SEC	10
#define UI_REQ_WAIT_SEC		10
#define NET_CONN_WAIT_MIN	15

/* Provide a buffer large enough to hold the full HTTPS REST response */
#define REST_RX_BUF_SZ		1024

/* Modem FW version required to properly run this sample */
#define MFWV_MAJ_SAMPLE_REQ	1
#define MFWV_MIN_SAMPLE_REQ	3
#define MFWV_REV_SAMPLE_REQ	0
/* Modem FW version required for extended neighbor cells search */
#define MFWV_MAJ_EXT_SRCH	1
#define MFWV_MIN_EXT_SRCH	3
#define MFWV_REV_EXT_SRCH	1

/* Semaphore to indicate a button has been pressed */
static K_SEM_DEFINE(button_press_sem, 0, 1);

/* Semaphore to indicate that a network connection has been established */
static K_SEM_DEFINE(lte_connected_sem, 0, 1);

/* Semaphore to indicate that cell info has been received */
static K_SEM_DEFINE(cell_info_ready_sem, 0, 1);

/* Mutex for cell info struct */
static K_MUTEX_DEFINE(cell_info_mutex);

/* nRF Cloud device ID */
static char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];

/* Buffer used for REST calls */
static char rx_buf[REST_RX_BUF_SZ];

/* nRF Cloud REST context */
static struct nrf_cloud_rest_context rest_ctx = {
	.connect_socket = -1,
	.rx_buf = rx_buf,
	.rx_buf_len = sizeof(rx_buf),
	.fragment_size = 0,
	/* A JWT will be automatically generated using the configured nRF Cloud device ID.
	 * See CONFIG_NRF_CLOUD_REST_AUTOGEN_JWT and CONFIG_NRF_CLOUD_CLIENT_ID_SRC.
	 */
	.auth = NULL
};

/* Type of data to be sent in the cellular positioning request */
enum nrf_cloud_location_type active_cell_pos_type = LOCATION_TYPE_SINGLE_CELL;

/* Search type used for neighbor cell measurements; modem FW version depenedent */
static enum lte_lc_neighbor_search_type search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_DEFAULT;

/* Buffer to hold neighbor cell measurement data for multi-cell requests */
static struct lte_lc_ncell neighbor_cells[CONFIG_LTE_NEIGHBOR_CELLS_MAX];

/* Modem info struct used for modem FW version and cell info used for single-cell requests */
static struct modem_param_info mdm_param;

/* Structure to hold all cell info */
static struct lte_lc_cells_info cell_info;

/* REST request structure to contain cell info to be sent to nRF Cloud */
static struct nrf_cloud_rest_location_request cell_pos_req = {
	.cell_info = &cell_info,
	.wifi_info = NULL
};

/* Flag to indicate that the device is ready to perform requests */
static bool ready;

/* Flag to indicate if the device shadow should be updated so that the location card
 * is displayed on nrfcloud.com
 */
static bool location_card_enable;

#if defined(CONFIG_REST_CELL_LOCATION_DO_JITP)
/* Flag to indicate if the user requested JITP to be performed */
static bool jitp_requested;
#endif

/* Register a listener for application events, specifically a button event */
static bool app_event_handler(const struct app_event_header *aeh);
APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, button_event);

static bool ver_check(int32_t reqd_maj, int32_t reqd_min, int32_t reqd_rev,
		      int32_t maj, int32_t min, int32_t rev)
{
	if (maj > reqd_maj) {
		return true;
	} else if ((maj == reqd_maj) && (min > reqd_min)) {
		return true;
	} else if ((maj == reqd_maj) && (min == reqd_min) && (rev >= reqd_rev)) {
		return true;
	}
	return false;
}

static void check_modem_fw_version(void)
{
	char mfwv_str[128];
	uint32_t major;
	uint32_t minor;
	uint32_t rev;

	if (modem_info_string_get(MODEM_INFO_FW_VERSION, mfwv_str, sizeof(mfwv_str)) <= 0) {
		LOG_WRN("Failed to get modem FW version");
		return;
	}

	LOG_INF("Modem FW version: %s", mfwv_str);

	if (sscanf(mfwv_str, "mfw_nrf9160_%u.%u.%u", &major, &minor, &rev) != 3) {
		LOG_WRN("Unable to parse modem FW version number");
		return;
	}

	/* Ensure the modem firmware version meets the requirement for this sample */
	if (!ver_check(MFWV_MAJ_SAMPLE_REQ, MFWV_MIN_SAMPLE_REQ, MFWV_REV_SAMPLE_REQ,
		       major, minor, rev)) {
		LOG_ERR("This sample requires modem FW version %d.%d.%d or later",
			MFWV_MAJ_SAMPLE_REQ, MFWV_MIN_SAMPLE_REQ, MFWV_REV_SAMPLE_REQ);
		LOG_INF("Update modem firmware and restart");
		k_sleep(K_FOREVER);
	}

	/* Enable extended search if modem fw version allows */
	if (ver_check(MFWV_MAJ_EXT_SRCH, MFWV_MIN_EXT_SRCH, MFWV_REV_EXT_SRCH,
		       major, minor, rev)) {
		search_type = LTE_LC_NEIGHBOR_SEARCH_TYPE_EXTENDED_COMPLETE;
	}
}

static void get_cell_info(void)
{
	if (!ready) {
		return;
	}

	int err;

	if (active_cell_pos_type == LOCATION_TYPE_SINGLE_CELL) {
		LOG_INF("Getting current cell info...");

		/* Use the modem info library to easily obtain the required network info
		 * for a single-cell request without performing a neighbor cell measurement
		 */
		err = modem_info_params_get(&mdm_param);
		if (err) {
			LOG_ERR("Unable to obtain modem info, error: %d", err);
			return;
		}

		(void)k_mutex_lock(&cell_info_mutex, K_FOREVER);
		memset(&cell_info, 0, sizeof(cell_info));
		/* Required parameters */
		cell_info.current_cell.id	= (uint32_t)mdm_param.network.cellid_dec;
		cell_info.current_cell.mcc	= mdm_param.network.mcc.value;
		cell_info.current_cell.mnc	= mdm_param.network.mnc.value;
		cell_info.current_cell.tac	= mdm_param.network.area_code.value;
		/* Optional */
		cell_info.current_cell.rsrp	= mdm_param.network.rsrp.value;
		/* Omitted - optional parameters not available from modem_info */
		cell_info.current_cell.timing_advance	= NRF_CLOUD_LOCATION_CELL_OMIT_TIME_ADV;
		cell_info.current_cell.rsrq		= NRF_CLOUD_LOCATION_CELL_OMIT_RSRQ;
		cell_info.current_cell.earfcn		= NRF_CLOUD_LOCATION_CELL_OMIT_EARFCN;
		(void)k_mutex_unlock(&cell_info_mutex);

		k_sem_give(&cell_info_ready_sem);

	} else {
		struct lte_lc_ncellmeas_params ncellmeas_params = {
			.search_type = search_type,
		};

		LOG_INF("Requesting neighbor cell measurement...");
		cell_info.neighbor_cells = neighbor_cells;
		err = lte_lc_neighbor_cell_measurement(&ncellmeas_params);
		if (err) {
			LOG_ERR("Failed to start neighbor cell measurement, error: %d", err);
		}
	}
}

static bool app_event_handler(const struct app_event_header *aeh)
{
	struct button_event *btn_evt = is_button_event(aeh) ? cast_button_event(aeh) : NULL;

	if (!btn_evt) {
		return false;
	} else if (!btn_evt->pressed) {
		return true;
	}

	LOG_DBG("Button pressed");

	if (ready) {
		/* Toggle active cell pos type */
		if (active_cell_pos_type == LOCATION_TYPE_SINGLE_CELL) {
			active_cell_pos_type = LOCATION_TYPE_MULTI_CELL;
		} else {
			active_cell_pos_type = LOCATION_TYPE_SINGLE_CELL;
		}

		/* Get latest cell info on button press */
		get_cell_info();
	}

	k_sem_give(&button_press_sem);

	return true;
}

static void modem_time_wait(void)
{
	int err;
	char time_buf[64];

	LOG_INF("Waiting for modem to acquire network time...");

	do {
		k_sleep(K_SECONDS(3));
		err = nrf_modem_at_cmd(time_buf, sizeof(time_buf), "AT%%CCLK?");
	} while (err != 0);

	LOG_INF("Network time obtained");
}

#if defined(CONFIG_REST_CELL_LOCATION_DO_JITP)
static void request_jitp(void)
{
	int err;

	k_sem_reset(&button_press_sem);

	LOG_INF("---> Press button %d to request just-in-time provisioning (JITP)",
		BTN_NUM);
	LOG_INF("     This only needs to be done once per device");
	LOG_INF("     Waiting %d seconds...", JITP_REQ_WAIT_SEC);

	err = k_sem_take(&button_press_sem, K_SECONDS(JITP_REQ_WAIT_SEC));
	if (err == 0) {
		jitp_requested = true;
		LOG_INF("JITP will be performed after network connection is obtained");
		/* Enable the location card for a newly JITP'd device */
		location_card_enable = true;
	} else {
		if (err != -EAGAIN) {
			LOG_ERR("k_sem_take error: %d", err);
		}

		LOG_INF("JITP will be skipped");
	}
}

static void do_jitp(void)
{
	int err = 0;

	LOG_INF("Performing JITP...");
	err = nrf_cloud_rest_jitp(CONFIG_NRF_CLOUD_SEC_TAG);

	if (err == 0) {
		LOG_INF("Waiting 30s for cloud provisioning to complete...");
		k_sleep(K_SECONDS(30));
		k_sem_reset(&button_press_sem);
		LOG_INF("Associate device with nRF Cloud account and press button %d when complete",
			BTN_NUM);
		(void)k_sem_take(&button_press_sem, K_FOREVER);
	} else if (err == 1) {
		LOG_INF("Device already provisioned");
	} else {
		LOG_ERR("Device provisioning failed");
	}
}
#endif

#define REQ_CARD "---> Press button %d to enable the location card for this device on nrfcloud.com"
static void request_location_card_enable(void)
{
	if (!location_card_enable) {
		int err;

		k_sem_reset(&button_press_sem);

		LOG_INF(REQ_CARD, BTN_NUM);
		LOG_INF("     This only needs to be done once per device");
		LOG_INF("     Waiting %d seconds...", UI_REQ_WAIT_SEC);

		err = k_sem_take(&button_press_sem, K_SECONDS(UI_REQ_WAIT_SEC));

		if (err == 0) {
			location_card_enable = true;
		} else {
			if (err != -EAGAIN) {
				LOG_ERR("k_sem_take error: %d", err);
			}
		}
	}

	if (location_card_enable) {
		LOG_INF("Location card will be enabled after network connection is obtained");
	} else {
		LOG_INF("Location card will not be enabled");
	}
}

static void do_location_card_enable(void)
{
	int err;

	struct nrf_cloud_svc_info_ui ui = {
		.gnss = 1
	};
	struct nrf_cloud_svc_info svc_inf = {
		.fota = NULL,
		.ui = &ui
	};

	/* Keep the connection alive so it can be used by the initial cellular
	 * positioning request.
	 */
	rest_ctx.keep_alive = true;

	LOG_INF("Enabling location card...");
	err = nrf_cloud_rest_shadow_service_info_update(&rest_ctx, device_id, &svc_inf);
	if (err) {
		LOG_ERR("Failed to enable location card, error: %d", err);
	} else {
		LOG_INF("Location card enabled in device shadow");
	}

	/* Set keep alive to false so the connection is closed after the initial
	 * positioning request. The subsequent request may not occur immediately.
	 */
	rest_ctx.keep_alive = false;
}

int init(void)
{
	int err;

	/* Application event manager is used for button press events from the
	 * common application framework (CAF) library
	 */
	err = app_event_manager_init();
	if (err) {
		LOG_ERR("Application Event Manager could not be initialized");
		return err;
	}

	/* Modem info library is used to obtain the modem FW version
	 * and network info for single-cell requests
	 */
	err = modem_info_init();
	if (err) {
		LOG_ERR("Modem info initialization failed, error: %d", err);
		return err;
	}

	err = modem_info_params_init(&mdm_param);
	if (err) {
		LOG_ERR("Modem info params initialization failed, error: %d", err);
		return err;
	}

	/* Check modem FW version */
	check_modem_fw_version();

	/* Print the nRF Cloud device ID. This device ID should match the ID used
	 * to provision the device on nRF Cloud or to register a public JWT signing key.
	 */
	err = nrf_cloud_client_id_get(device_id, sizeof(device_id));
	if (err) {
		LOG_ERR("Failed to get device ID, error: %d", err);
		return err;
	}

	LOG_INF("Device ID: %s", device_id);

	/* Inform the app event manager that this module is ready to receive events */
	module_set_state(MODULE_STATE_READY);

#if defined(CONFIG_REST_CELL_LOCATION_DO_JITP)
	/* Present option for JITP via REST */
	request_jitp();
#endif

	return 0;
}

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_DBG("LTE_LC_EVT_NW_REG_STATUS: %d", evt->nw_reg_status);
		if ((evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_HOME) &&
		    (evt->nw_reg_status != LTE_LC_NW_REG_REGISTERED_ROAMING)) {
			if (ready) {
				LOG_ERR("Disconnected from network, restart device to reconnect");
			}
			break;
		}

		LOG_DBG("Network registration status: %s",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ?
			"Connected - home network" : "Connected - roaming");

		k_sem_give(&lte_connected_sem);
		break;
	case LTE_LC_EVT_CELL_UPDATE:
		if (!ready || (evt->cell.id == LTE_LC_CELL_EUTRAN_ID_INVALID)) {
			break;
		}

		/* Get new info when cell ID changes */
		LOG_INF("Cell info changed");
		get_cell_info();

		break;
	case LTE_LC_EVT_NEIGHBOR_CELL_MEAS:
		if (evt->cells_info.current_cell.id == LTE_LC_CELL_EUTRAN_ID_INVALID) {
			LOG_WRN("Current cell ID not valid in neighbor cell measurement results");
			break;
		}

		(void)k_mutex_lock(&cell_info_mutex, K_FOREVER);
		/* Copy current cell information. */
		memcpy(&cell_info.current_cell,
		       &evt->cells_info.current_cell,
		       sizeof(cell_info.current_cell));

		cell_info.ncells_count = evt->cells_info.ncells_count;

		/* Copy neighbor cell information if present. */
		if ((evt->cells_info.ncells_count > 0) && (evt->cells_info.neighbor_cells)) {
			memcpy(neighbor_cells,
			       evt->cells_info.neighbor_cells,
			       sizeof(neighbor_cells[0]) * cell_info.ncells_count);
			LOG_INF("Received measurements for %u neighbor cells",
				cell_info.ncells_count);
		} else {
			LOG_WRN("No neighbor cells were measured");
		}
		(void)k_mutex_unlock(&cell_info_mutex);
		k_sem_give(&cell_info_ready_sem);

		break;
	default:
		break;
	}
}

static void connect_to_network(void)
{
	int err;

	LOG_INF("Waiting for network...");

	k_sem_reset(&lte_connected_sem);

	err = lte_lc_init_and_connect_async(lte_handler);
	if (err) {
		LOG_ERR("Failed to init LTE module, unable to continue, error: %d", err);
		k_sleep(K_FOREVER);
	}

	/* While waiting, ask about the location card */
	request_location_card_enable();

	err = k_sem_take(&lte_connected_sem, K_MINUTES(NET_CONN_WAIT_MIN));
	if (err == 0) {
		LOG_INF("Connected to network");
	} else if (err == -EAGAIN) {
		LOG_ERR("Failed to connect to network, rebooting in 30s...");
		(void)lte_lc_deinit();
		k_sleep(K_SECONDS(30));
		sys_reboot(SYS_REBOOT_COLD);
	} else {
		LOG_ERR("k_sem_take error %d, unable to continue", err);
		k_sleep(K_FOREVER);
	}

	/* Modem must have valid time/date in order to generate JWTs with
	 * an expiration time
	 */
	modem_time_wait();
}

void main(void)
{
	int err;

	LOG_INF("nRF Cloud REST Cellular Location Sample");

	err = init();
	if (err) {
		LOG_ERR("Initialization failed");
		return;
	}

	connect_to_network();

#if defined(CONFIG_REST_CELL_LOCATION_DO_JITP)
	if (jitp_requested) {
		/* Perform JITP via REST */
		do_jitp();
	}
#endif
	if (location_card_enable) {
		do_location_card_enable();
	}

	/* Initialized, connected, and ready to send cellular location requests */
	ready = true;

	/* Get initial cell info */
	get_cell_info();

	while (1) {
		struct nrf_cloud_location_result cell_pos_result;

		/* Wait for cell info to become available; triggered by
		 * a button press or a change in cell ID.
		 */
		(void)k_sem_take(&cell_info_ready_sem, K_FOREVER);

		(void)k_mutex_lock(&cell_info_mutex, K_FOREVER);

		LOG_INF("Current cell info: Cell ID: %u, TAC: %u, MCC: %d, MNC: %d",
			cell_info.current_cell.id, cell_info.current_cell.tac,
			cell_info.current_cell.mcc, cell_info.current_cell.mnc);

		if (cell_info.ncells_count) {
			LOG_INF("Performing multi-cell request with %u neighbor cells",
				cell_info.ncells_count);
		} else {
			LOG_INF("Performing single-cell request");
		}

		/* Perform REST call */
		err = nrf_cloud_rest_location_get(&rest_ctx, &cell_pos_req, &cell_pos_result);

		(void)k_mutex_unlock(&cell_info_mutex);

		if (err) {
			LOG_ERR("Request failed, error: %d", err);
			if (cell_pos_result.err != NRF_CLOUD_ERROR_NONE) {
				LOG_ERR("nRF Cloud error code: %d", cell_pos_result.err);
			}
			continue;
		}

		LOG_INF("Cellular location request fulfilled with %s",
			cell_pos_result.type == LOCATION_TYPE_SINGLE_CELL ? "single-cell" :
			cell_pos_result.type == LOCATION_TYPE_MULTI_CELL ?  "multi-cell" :
									      "unknown");

		LOG_INF("Lat: %f, Lon: %f, Uncertainty: %u",
			cell_pos_result.lat,
			cell_pos_result.lon,
			cell_pos_result.unc);
	}
}
