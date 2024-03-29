/**
 * Copyright (c) 2016 - 2017, Nordic Semiconductor ASA
 * 
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 * 
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 * 
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 * 
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 * 
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "nordic_common.h"
#include "app_error.h"
#include "app_uart.h"
#include "ble_db_discovery.h"
#include "app_timer.h"
#include "app_util.h"
#include "bsp_btn_ble.h"
#include "ble.h"
#include "ble_gap.h"
#include "ble_hci.h"
#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "nrf_pwr_mgmt.h"
#include "ble_advdata.h"
#include "ble_nus_c.h"
#include "nrf_ble_gatt.h"

#include "nrf_fstorage.h"
#include "nrf_fstorage_sd.h"

#include "nrf_log.h"
#include "nrf_log_ctrl.h"
#include "nrf_log_default_backends.h"


#define APP_BLE_CONN_CFG_TAG    1                                       /**< A tag that refers to the BLE stack configuration we set with @ref sd_ble_cfg_set. Default tag is @ref BLE_CONN_CFG_TAG_DEFAULT. */
#define APP_BLE_OBSERVER_PRIO   3                                       /**< Application's BLE observer priority. You shoulnd't need to modify this value. */

#define UART_TX_BUF_SIZE        256                                     /**< UART TX buffer size. */
#define UART_RX_BUF_SIZE        256                                     /**< UART RX buffer size. */

#define NUS_SERVICE_UUID_TYPE   BLE_UUID_TYPE_VENDOR_BEGIN              /**< UUID type for the Nordic UART Service (vendor specific). */

#define SCAN_INTERVAL           0x00A0                                  /**< Determines scan interval in units of 0.625 millisecond. */
#define SCAN_WINDOW             0x0090                                  /**< Determines scan window in units of 0.625 millisecond. */
#define SCAN_TIMEOUT            0x0000                                  /**< Timout when scanning. 0x0000 disables timeout. */

#define MIN_CONNECTION_INTERVAL MSEC_TO_UNITS(100, UNIT_1_25_MS)         /**< Determines minimum connection interval in millisecond. */
#define MAX_CONNECTION_INTERVAL MSEC_TO_UNITS(100, UNIT_1_25_MS)         /**< Determines maximum connection interval in millisecond. */
#define SLAVE_LATENCY           0                                       /**< Determines slave latency in counts of connection events. */
#define SUPERVISION_TIMEOUT     MSEC_TO_UNITS(4000, UNIT_10_MS)         /**< Determines supervision time-out in units of 10 millisecond. */

#define UUID16_SIZE             2                                       /**< Size of 16 bit UUID */
#define UUID32_SIZE             4                                       /**< Size of 32 bit UUID */
#define UUID128_SIZE            16                                      /**< Size of 128 bit UUID */

#define BLE_EHSB_SERVICE        0x0001

#define STOP_SIGN               22

APP_TIMER_DEF(m_led_timer_id);                                           //  Macro for timer id
APP_TIMER_DEF(m_add_uuid_timer_id);
APP_TIMER_DEF(m_erase_whitelist_timer_id);

BLE_NUS_C_DEF(m_ble_nus_c);                                             /**< BLE NUS service client instance. */
NRF_BLE_GATT_DEF(m_gatt);                                               /**< GATT module instance. */
BLE_DB_DISCOVERY_DEF(m_db_disc);                                        /**< DB discovery module instance. */

static uint16_t m_ble_nus_max_data_len = BLE_GATT_ATT_MTU_DEFAULT - OPCODE_LENGTH - HANDLE_LENGTH; /**< Maximum length of data (in bytes) that can be transmitted to the peer by the Nordic UART service module. */

/* Initiating different bools used to ensure smooth running*/
bool reset = false;
bool scanning = false;
bool add_uuid = false;
bool erasing_whitelist = false;
bool new_uuid_added = false;
bool existing_uuid = false;
bool connected = false;
bool whitelist_erased = false;

/* Variables used for "whitelisting" stop button UUIDs*/
uint8_t uuid_number = 0;
uint32_t flash_addr = 0x3f000;
uint8_t delete_counter = 0;
/* Two-dimensional array to hold the UUIDs that should be "whitelisted" */
uint8_t whitelist[30][16] = {0};

static void fstorage_evt_handler(nrf_fstorage_evt_t * p_evt);

/**NRF FSTORAGE instance*/
NRF_FSTORAGE_DEF(nrf_fstorage_t whitelist_storage) =
{
    /* Set a handler for fstorage events. */
    .evt_handler = fstorage_evt_handler,
    .start_addr = 0x3e000,
    .end_addr   = 0x3ffff,
}; 

/**@brief Connection parameters requested for connection. */
static ble_gap_conn_params_t const m_connection_param =
{
    (uint16_t)MIN_CONNECTION_INTERVAL,  // Minimum connection
    (uint16_t)MAX_CONNECTION_INTERVAL,  // Maximum connection
    (uint16_t)SLAVE_LATENCY,            // Slave latency
    (uint16_t)SUPERVISION_TIMEOUT       // Supervision time-out
};

/** @brief Parameters used when scanning. */
static ble_gap_scan_params_t const m_scan_params =
{
    .active   = 1,
    .interval = SCAN_INTERVAL,
    .window   = SCAN_WINDOW,
    .timeout  = SCAN_TIMEOUT,
    #if (NRF_SD_BLE_API_VERSION <= 2)
        .selective   = 0,
        .p_whitelist = NULL,
    #endif
    #if (NRF_SD_BLE_API_VERSION >= 3)
        .use_whitelist = 0,
    #endif
};

/**@brief NUS uuid. */
static ble_uuid_t const m_nus_uuid =
{
    .uuid = BLE_UUID_NUS_SERVICE,
    .type = NUS_SERVICE_UUID_TYPE
};

/**@brief Function for asserts in the SoftDevice.
 *
 * @details This function will be called in case of an assert in the SoftDevice.
 *
 * @warning This handler is an example only and does not fit a final product. You need to analyze
 *          how your product is supposed to react in case of Assert.
 * @warning On assert from the SoftDevice, the system can only recover on reset.
 *
 * @param[in] line_num     Line number of the failing ASSERT call.
 * @param[in] p_file_name  File name of the failing ASSERT call.
 */
void assert_nrf_callback(uint16_t line_num, const uint8_t * p_file_name)
{
    app_error_handler(0xDEADBEEF, line_num, p_file_name);
}

/** Toggle LED 1 to visualize scanning*/
static void led_timeout_handler(void * p_context)
{
  nrf_gpio_pin_toggle(LED_1);
}

/** Toggle LED 4 to show that the central is in "add UUID-mode"*/
static void add_uuid_timeout_handler(void * p_context)
{
  nrf_gpio_pin_toggle(LED_4);
}

/** Toggle all LEDs to show that the "whitelist" will be erased
    and erase after 4 seconds*/
static void erase_uuids_timeout_handler(void * p_context)
{
  nrf_gpio_pin_toggle(LED_1);
  nrf_gpio_pin_toggle(LED_2);
  nrf_gpio_pin_toggle(LED_3);
  nrf_gpio_pin_toggle(LED_4);

  delete_counter += 1;

  if(delete_counter == 17)
  {
      nrf_fstorage_erase(&whitelist_storage, 0x3e000, 1, NULL);
      nrf_fstorage_erase(&whitelist_storage, 0x3f000, 1, NULL);
      app_timer_stop(m_erase_whitelist_timer_id);
      uuid_number = 0;
      flash_addr = 0x3f000;
      whitelist_erased = true;
      NRF_LOG_INFO("Deleted");
  }
}

/**@brief Function to start scanning. */
static void scan_start(void)
{
    ret_code_t ret;

    ret = sd_ble_gap_scan_start(&m_scan_params);
    APP_ERROR_CHECK(ret);

    app_timer_start(m_led_timer_id, APP_TIMER_TICKS(1000), led_timeout_handler);
    scanning = true;
}

/**@brief Function to stop scanning. */
static void scan_stop(void)
{
    sd_ble_gap_scan_stop();
    app_timer_stop(m_led_timer_id);
    nrf_gpio_pin_set(LED_1);
    scanning = false;
}

/** Function for handling fstorage events*/
static void fstorage_evt_handler(nrf_fstorage_evt_t * p_evt) 
{
    if (p_evt->result != NRF_SUCCESS)
    {
        NRF_LOG_INFO("--> Event received: ERROR while executing an fstorage operation.");
        return;
    }

    switch (p_evt->id)
    {
        case NRF_FSTORAGE_EVT_WRITE_RESULT:
        {
            NRF_LOG_INFO("--> Event received: wrote %d bytes at address 0x%x.",
                         p_evt->len, p_evt->addr);
        } break;

        case NRF_FSTORAGE_EVT_ERASE_RESULT:
        {
            NRF_LOG_INFO("--> Event received: erased %d page from address 0x%x.",
                         p_evt->len, p_evt->addr);
        } break;

        default:
            break;
    }
}

/**@brief Function for handling database discovery events.
 *
 * @details This function is callback function to handle events from the database discovery module.
 *          Depending on the UUIDs that are discovered, this function should forward the events
 *          to their respective services.
 *
 * @param[in] p_event  Pointer to the database discovery event.
 */
static void db_disc_handler(ble_db_discovery_evt_t * p_evt)
{
    ble_nus_c_on_db_disc_evt(&m_ble_nus_c, p_evt);
}


/**@brief Function for handling characters received by the Nordic UART Service.
 *
 * @details This function takes a list of characters of length data_len and prints the characters out on UART.
 *          If @ref ECHOBACK_BLE_UART_DATA is set, the data is sent back to sender.
 */
static void ble_nus_chars_received_uart_print(uint8_t * p_data, uint16_t data_len)
{
    ret_code_t ret_val;

    NRF_LOG_DEBUG("Receiving data.");
    NRF_LOG_HEXDUMP_DEBUG(p_data, data_len);

    for (uint32_t i = 0; i < data_len; i++)
    {
        do
        {
            ret_val = app_uart_put(p_data[i]);
            if ((ret_val != NRF_SUCCESS) && (ret_val != NRF_ERROR_BUSY))
            {
                NRF_LOG_ERROR("app_uart_put failed for index 0x%04x.", i);
                APP_ERROR_CHECK(ret_val);
            }
        } while (ret_val == NRF_ERROR_BUSY);
    }
    if (p_data[data_len-1] == '\r')
    {
        while (app_uart_put('\n') == NRF_ERROR_BUSY);
    }
}


/**@brief   Function for handling app_uart events.
 *
 * @details This function will receive a single character from the app_uart module and append it to
 *          a string. The string will be be sent over BLE when the last character received was a
 *          'new line' '\n' (hex 0x0A) or if the string has reached the maximum data length.
 */
void uart_event_handle(app_uart_evt_t * p_event)
{
    static uint8_t data_array[BLE_NUS_MAX_DATA_LEN];
    static uint16_t index = 0;
    uint32_t ret_val;

    switch (p_event->evt_type)
    {
        /**@snippet [Handling data from UART] */
        case APP_UART_DATA_READY:
            UNUSED_VARIABLE(app_uart_get(&data_array[index]));
            index++;

            if ((data_array[index - 1] == '\n') || (index >= (m_ble_nus_max_data_len)))
            {
                NRF_LOG_DEBUG("Ready to send data over BLE NUS");
                NRF_LOG_HEXDUMP_DEBUG(data_array, index);

                do
                {
                    ret_val = ble_nus_c_string_send(&m_ble_nus_c, data_array, index);
                    if ( (ret_val != NRF_ERROR_INVALID_STATE) && (ret_val != NRF_ERROR_BUSY) )
                    {
                        APP_ERROR_CHECK(ret_val);
                    }
                } while (ret_val == NRF_ERROR_BUSY);

                index = 0;
            }
            break;

        /**@snippet [Handling data from UART] */
        case APP_UART_COMMUNICATION_ERROR:
            NRF_LOG_ERROR("Communication error occurred while handling UART.");
            APP_ERROR_HANDLER(p_event->data.error_communication);
            break;

        case APP_UART_FIFO_ERROR:
            NRF_LOG_ERROR("Error occurred in FIFO module used by UART.");
            APP_ERROR_HANDLER(p_event->data.error_code);
            break;

        default:
            break;
    }
}

/**@brief Callback handling NUS Client events.
 *
 * @details This function is called to notify the application of NUS client events.
 *
 * @param[in]   p_ble_nus_c   NUS Client Handle. This identifies the NUS client
 * @param[in]   p_ble_nus_evt Pointer to the NUS Client event.
 */

/**@snippet [Handling events from the ble_nus_c module] */
static void ble_nus_c_evt_handler(ble_nus_c_t * p_ble_nus_c, ble_nus_c_evt_t const * p_ble_nus_evt)
{
    ret_code_t err_code;

    switch (p_ble_nus_evt->evt_type)
    {        
        case BLE_NUS_C_EVT_DISCOVERY_COMPLETE:
            NRF_LOG_INFO("Discovery complete.");
            err_code = ble_nus_c_handles_assign(p_ble_nus_c, p_ble_nus_evt->conn_handle, &p_ble_nus_evt->handles);
            APP_ERROR_CHECK(err_code);

            err_code = ble_nus_c_tx_notif_enable(p_ble_nus_c);
            APP_ERROR_CHECK(err_code);
            NRF_LOG_INFO("Connected to device with Nordic UART Service.");

            //Relayer connected, start scan for stop buttons
            scan_start();
 
            nrf_gpio_pin_clear(LED_3);
            connected = true;
            break;

        case BLE_NUS_C_EVT_NUS_TX_EVT:
            ble_nus_chars_received_uart_print(p_ble_nus_evt->p_data, p_ble_nus_evt->data_len);
            /**UUID received from relayer. Compare it to "whitelist"*/
            if(p_ble_nus_evt->data_len == 16 && !add_uuid && !erasing_whitelist && !reset)
            {
                  for(uint8_t i = 0; i < uuid_number; i++)
                  {
                      if(memcmp(p_ble_nus_evt->p_data, whitelist[i], 16) == 0)
                      {
                          nrf_gpio_pin_set(STOP_SIGN);
                          nrf_gpio_pin_clear(LED_4);
                          reset = true;
                      }
                  }
            }
            break;

        case BLE_NUS_C_EVT_DISCONNECTED:
            NRF_LOG_INFO("Disconnected.");
            nrf_gpio_pin_set(LED_3);
            connected = false;
            break;
    }
}

/**@brief Reads an advertising report and checks if a UUID is present in the service list.
 *
 * @details The function is able to search for 128-bit service UUIDs.
 *          To see the format of a advertisement packet, see
 *          https://www.bluetooth.org/Technical/AssignedNumbers/generic_access_profile.htm
 *
 * @param[in]   p_target_uuid The UUID to search for.
 * @param[in]   p_adv_report  Pointer to the advertisement report.
 *
 * @retval      true if the UUID is present in the advertisement report. Otherwise false
 */
static bool is_uuid_present(ble_uuid_t               const * p_target_uuid,
                            ble_gap_evt_adv_report_t const * p_adv_report)
{
    ret_code_t   err_code;
    ble_uuid_t   extracted_uuid;
    uint16_t     index  = 0;
    uint8_t    * p_data = (uint8_t *)p_adv_report->data;

    while (index < p_adv_report->dlen)
    {
        uint8_t field_length = p_data[index];
        uint8_t field_type   = p_data[index + 1];

        if (   (field_type == BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_MORE_AVAILABLE)
                 || (field_type == BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE))
        {

            err_code = sd_ble_uuid_decode(UUID128_SIZE, &p_data[index + 2], &extracted_uuid);
            if (err_code == NRF_SUCCESS)
            {

                if (   (extracted_uuid.uuid == p_target_uuid->uuid)
                    && (extracted_uuid.type == p_target_uuid->type))
                {
                    return true;
                }
            }
        }
        index += field_length + 1;
    }
    return false;
}


/**@brief Function for handling BLE events.
 *
 * @param[in]   p_ble_evt   Bluetooth stack event.
 * @param[in]   p_context   Unused.
 */
static void ble_evt_handler(ble_evt_t const * p_ble_evt, void * p_context)
{
    ret_code_t            err_code;
    ble_gap_evt_t const * p_gap_evt = &p_ble_evt->evt.gap_evt;

    switch (p_ble_evt->header.evt_id)
    {
        case BLE_GAP_EVT_ADV_REPORT:
        {
           ble_gap_evt_adv_report_t const * p_adv_report = &p_gap_evt->params.adv_report;
            
           uint8_t adv_uuid[16] = {0};
           memcpy(&adv_uuid, &p_adv_report->data[5], 16);

            if(!add_uuid)
            {   
                //If not connected to relayer, check for relayer-UUID and connect if found
                if(!connected)
                {
                    if (is_uuid_present(&m_nus_uuid, p_adv_report))
                    {
                        err_code = sd_ble_gap_connect(&p_adv_report->peer_addr,
                                                      &m_scan_params,
                                                      &m_connection_param,
                                                      APP_BLE_CONN_CFG_TAG);
                        if (err_code == NRF_SUCCESS)
                        {
                            // scan is automatically stopped by the connect
                            NRF_LOG_INFO("Connecting to target %02x%02x%02x%02x%02x%02x",
                                     p_adv_report->peer_addr.addr[0],
                                     p_adv_report->peer_addr.addr[1],
                                     p_adv_report->peer_addr.addr[2],
                                     p_adv_report->peer_addr.addr[3],
                                     p_adv_report->peer_addr.addr[4],
                                     p_adv_report->peer_addr.addr[5]
                                     );
                        }
                    }
                }
           
                if(!reset)
                {
                    //Compare received UUID to the entries in "whitelist
                    if(p_adv_report->data[2] == BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED \
                       && p_adv_report->data[4] == BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE)
                    {
                         for(int8_t i = 0; i < uuid_number; i++)
                         {                                                
                            if(memcmp(adv_uuid, whitelist[i], sizeof(adv_uuid)) == 0)
                            {
                                nrf_gpio_pin_set(STOP_SIGN);
                                nrf_gpio_pin_clear(LED_2);
                                reset = true;
                            }
                         }
                    }
                }
            }
            else if(add_uuid)
            {
                //Check RSSI, flag and UUID type to ensure that advertising device is close and of right kind.
                if(p_ble_evt->evt.gap_evt.params.adv_report.rssi > -35 \
                   && p_adv_report->data[2] == BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED \
                   && p_adv_report->data[4] == BLE_GAP_AD_TYPE_128BIT_SERVICE_UUID_COMPLETE)
                {
                      for(uint8_t i = 0; i < uuid_number; i++)
                      {
                          if(memcmp(adv_uuid, whitelist[i], sizeof(adv_uuid)) == 0)
                          {
                              app_timer_stop(m_add_uuid_timer_id);
                              nrf_gpio_pin_set(LED_4);
                              nrf_gpio_pin_clear(LED_2);
                              existing_uuid = true;
                          }
                      }
                      if(!new_uuid_added && !existing_uuid)
                      {
                          //Copy the UUID from advertisement report to the next slot in whitelist
                          memcpy(&whitelist[uuid_number], &p_adv_report->data[5], 16);

                          app_timer_stop(m_add_uuid_timer_id);
                          nrf_gpio_pin_clear(LED_4);
                          new_uuid_added = true;

                          NRF_LOG_INFO("Writing \"%x\" to flash.", whitelist[uuid_number]);
                          err_code = nrf_fstorage_write(&whitelist_storage, flash_addr, whitelist[uuid_number], sizeof(whitelist[uuid_number]), NULL);
                          APP_ERROR_CHECK(err_code);
                          NRF_LOG_INFO("Done.");

                          flash_addr += 0x10;
                      }             
                }
            }
        }break; // BLE_GAP_EVT_ADV_REPORT

        case BLE_GAP_EVT_CONNECTED:
            NRF_LOG_INFO("Connected to target");
            err_code = ble_nus_c_handles_assign(&m_ble_nus_c, p_ble_evt->evt.gap_evt.conn_handle, NULL);
            APP_ERROR_CHECK(err_code);

            // start discovery of services. The NUS Client waits for a discovery result
            err_code = ble_db_discovery_start(&m_db_disc, p_ble_evt->evt.gap_evt.conn_handle);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GAP_EVT_TIMEOUT:
            if (p_gap_evt->params.timeout.src == BLE_GAP_TIMEOUT_SRC_SCAN)
            {
                NRF_LOG_INFO("Scan timed out.");
                scan_start();
            }
            else if (p_gap_evt->params.timeout.src == BLE_GAP_TIMEOUT_SRC_CONN)
            {
                NRF_LOG_INFO("Connection Request timed out.");
            }
            break;

        case BLE_GAP_EVT_SEC_PARAMS_REQUEST:
            // Pairing not supported
            err_code = sd_ble_gap_sec_params_reply(p_ble_evt->evt.gap_evt.conn_handle, BLE_GAP_SEC_STATUS_PAIRING_NOT_SUPP, NULL, NULL);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST:
            // Accepting parameters requested by peer.
            err_code = sd_ble_gap_conn_param_update(p_gap_evt->conn_handle,
                                                    &p_gap_evt->params.conn_param_update_request.conn_params);
            APP_ERROR_CHECK(err_code);
            break;

#ifndef S140
        case BLE_GAP_EVT_PHY_UPDATE_REQUEST:
        {
            NRF_LOG_DEBUG("PHY update request.");
            ble_gap_phys_t const phys =
            {
                .rx_phys = BLE_GAP_PHY_AUTO,
                .tx_phys = BLE_GAP_PHY_AUTO,
            };
            err_code = sd_ble_gap_phy_update(p_ble_evt->evt.gap_evt.conn_handle, &phys);
            APP_ERROR_CHECK(err_code);
        } break;
#endif

        case BLE_GATTC_EVT_TIMEOUT:
            // Disconnect on GATT Client timeout event.
            NRF_LOG_DEBUG("GATT Client Timeout.");
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gattc_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        case BLE_GATTS_EVT_TIMEOUT:
            // Disconnect on GATT Server timeout event.
            NRF_LOG_DEBUG("GATT Server Timeout.");
            err_code = sd_ble_gap_disconnect(p_ble_evt->evt.gatts_evt.conn_handle,
                                             BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION);
            APP_ERROR_CHECK(err_code);
            break;

        default:
            break;
    }
}


/**@brief Function for initializing the BLE stack.
 *
 * @details Initializes the SoftDevice and the BLE event interrupt.
 */
static void ble_stack_init(void)
{
    ret_code_t err_code;

    err_code = nrf_sdh_enable_request();
    APP_ERROR_CHECK(err_code);

    // Configure the BLE stack using the default settings.
    // Fetch the start address of the application RAM.
    uint32_t ram_start = 0;
    err_code = nrf_sdh_ble_default_cfg_set(APP_BLE_CONN_CFG_TAG, &ram_start);
    APP_ERROR_CHECK(err_code);

    // Enable BLE stack.
    err_code = nrf_sdh_ble_enable(&ram_start);
    APP_ERROR_CHECK(err_code);

    // Register a handler for BLE events.
    NRF_SDH_BLE_OBSERVER(m_ble_observer, APP_BLE_OBSERVER_PRIO, ble_evt_handler, NULL);
}


/**@brief Function for handling events from the GATT library. */
void gatt_evt_handler(nrf_ble_gatt_t * p_gatt, nrf_ble_gatt_evt_t const * p_evt)
{
    if (p_evt->evt_id == NRF_BLE_GATT_EVT_ATT_MTU_UPDATED)
    {
        NRF_LOG_INFO("ATT MTU exchange completed.");

        m_ble_nus_max_data_len = p_evt->params.att_mtu_effective - OPCODE_LENGTH - HANDLE_LENGTH;
        NRF_LOG_INFO("Ble NUS max data length set to 0x%X(%d)", m_ble_nus_max_data_len, m_ble_nus_max_data_len);
    }
}


/**@brief Function for initializing the GATT library. */
void gatt_init(void)
{
    ret_code_t err_code;

    err_code = nrf_ble_gatt_init(&m_gatt, gatt_evt_handler);
    APP_ERROR_CHECK(err_code);

    err_code = nrf_ble_gatt_att_mtu_central_set(&m_gatt, NRF_SDH_BLE_GATT_MAX_MTU_SIZE);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing the UART. */
static void uart_init(void)
{
    ret_code_t err_code;

    app_uart_comm_params_t const comm_params =
    {
        .rx_pin_no    = RX_PIN_NUMBER,
        .tx_pin_no    = TX_PIN_NUMBER,
        .rts_pin_no   = RTS_PIN_NUMBER,
        .cts_pin_no   = CTS_PIN_NUMBER,
        .flow_control = APP_UART_FLOW_CONTROL_DISABLED,
        .use_parity   = false,
        .baud_rate    = UART_BAUDRATE_BAUDRATE_Baud115200
    };

    APP_UART_FIFO_INIT(&comm_params,
                       UART_RX_BUF_SIZE,
                       UART_TX_BUF_SIZE,
                       uart_event_handle,
                       APP_IRQ_PRIORITY_LOWEST,
                       err_code);

    APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing the NUS Client. */
static void nus_c_init(void)
{
    ret_code_t       err_code;
    ble_nus_c_init_t init;

    init.evt_handler = ble_nus_c_evt_handler;

    err_code = ble_nus_c_init(&m_ble_nus_c, &init);
    APP_ERROR_CHECK(err_code);
}

/** Function for handling button actions*/
void button_handler(uint8_t pin_no, uint8_t button_action)
{
   ret_code_t err_code;

   /**Button 1 turns of stop-sign and LEDs indicating button found*/
   if(pin_no == BUTTON_1 && button_action == APP_BUTTON_PUSH)
   {
      if(reset)
      {       
          nrf_gpio_pin_set(LED_4);
          nrf_gpio_pin_set(LED_2);
          nrf_gpio_pin_clear(STOP_SIGN);
          reset = false;
      }
   }
   /**Button 2 is for adding a new UUID to the system.
   A push of the button sets add_uuid = true
   and release sets add_uuid = false, meaning that the button 
   needs to be held down in order to add a new UUID*/
   if(pin_no == BUTTON_2 && button_action == APP_BUTTON_PUSH)
   {
       if(uuid_number > 30)
       {
          NRF_LOG_INFO("\"Whitelist\" full");
          nrf_gpio_pin_clear(LED_2);
          nrf_gpio_pin_clear(LED_4);
          existing_uuid = true;
       }
       else
       {
          app_timer_start(m_add_uuid_timer_id, APP_TIMER_TICKS(500), add_uuid_timeout_handler);
          add_uuid = true;
       }
   }
   if(pin_no == BUTTON_2 && button_action == APP_BUTTON_RELEASE)
   {
      app_timer_stop(m_add_uuid_timer_id);
      nrf_gpio_pin_set(LED_4);
      if(new_uuid_added)
      {
          uuid_number += 1;
          new_uuid_added = false;

          err_code = nrf_fstorage_erase(&whitelist_storage, 0x3e000, 1, NULL);
          APP_ERROR_CHECK(err_code);

          NRF_LOG_INFO("Writing \"%x\" to flash.", uuid_number);
          err_code = nrf_fstorage_write(&whitelist_storage, 0x3e000, &uuid_number, 4, NULL);
          APP_ERROR_CHECK(err_code);
      }

      else if(existing_uuid)
      {
          nrf_gpio_pin_set(LED_2);
          existing_uuid = false;
      }

      add_uuid = false;
   }
   /**Holding button 3 erases "whitelist" after 4 seconds of toggling all LEDs*/
   if(pin_no == BUTTON_3 && button_action == APP_BUTTON_PUSH)
   {
        erasing_whitelist = true;
        scan_stop();
        nrf_gpio_pin_set(LED_2);
        nrf_gpio_pin_set(LED_3);
        nrf_gpio_pin_set(LED_4);
        app_timer_start(m_erase_whitelist_timer_id, APP_TIMER_TICKS(250), erase_uuids_timeout_handler);
   }
   if(pin_no == BUTTON_3 && button_action == APP_BUTTON_RELEASE)
   {  
        if(!whitelist_erased)
        {
            app_timer_stop(m_erase_whitelist_timer_id);
            if(connected)
            {
                nrf_gpio_pin_clear(LED_3);
            }
        }
        nrf_gpio_pin_set(LED_2);
        nrf_gpio_pin_set(LED_4);
        if(!connected)
        {
            nrf_gpio_pin_set(LED_3);
        }
        scan_start();
        erasing_whitelist = false;
        delete_counter = 0;
   }
}

static void buttons_init()
{
    ret_code_t err_code;

    static app_button_cfg_t button_cfg[3] ={ {
        BUTTON_1,
        APP_BUTTON_ACTIVE_LOW,
        NRF_GPIO_PIN_PULLUP,
        button_handler
        },
        {
        BUTTON_2,
        APP_BUTTON_ACTIVE_LOW,
        NRF_GPIO_PIN_PULLUP,
        button_handler
        },
        {
        BUTTON_3,
        APP_BUTTON_ACTIVE_LOW,
        NRF_GPIO_PIN_PULLUP,
        button_handler
        } };

    err_code = app_button_init(button_cfg,3,5);
    APP_ERROR_CHECK(err_code);

    err_code = app_button_enable();
    APP_ERROR_CHECK(err_code);
}


/**@brief Function for initializing the timer. */
static void timer_init(void)
{
    ret_code_t err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);
}

static void application_timer_init(void)
{
    ret_code_t err_code;

    // Initialize timer module.
    err_code = app_timer_init();
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&m_led_timer_id, APP_TIMER_MODE_REPEATED, led_timeout_handler);
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&m_add_uuid_timer_id, APP_TIMER_MODE_REPEATED, add_uuid_timeout_handler);
    APP_ERROR_CHECK(err_code);

    err_code = app_timer_create(&m_erase_whitelist_timer_id, APP_TIMER_MODE_REPEATED, erase_uuids_timeout_handler);
    APP_ERROR_CHECK(err_code);
}

/**@brief Function for initializing the nrf log module. */
static void log_init(void)
{
    ret_code_t err_code = NRF_LOG_INIT(NULL);
    APP_ERROR_CHECK(err_code);

    NRF_LOG_DEFAULT_BACKENDS_INIT();
}


/**@brief Function for initializing the Power manager. */
static void power_init(void)
{
    ret_code_t err_code = nrf_pwr_mgmt_init();
    APP_ERROR_CHECK(err_code);
}


/** @brief Function for initializing the Database Discovery Module. */
static void db_discovery_init(void)
{
    ret_code_t err_code = ble_db_discovery_init(db_disc_handler);
    APP_ERROR_CHECK(err_code);
}

static void leds_init(void)
{
  for (int i=LED_1; i<= LED_4; i++)
  {
    nrf_gpio_cfg_output(i);
    nrf_gpio_pin_set(i);
  }
  nrf_gpio_cfg_output(STOP_SIGN);
  nrf_gpio_pin_clear(STOP_SIGN);
}

/**Function for reading UUIDs from flash and putting them in "whitelist" on start-up*/
static void read_flash(void)
{
    ret_code_t err_code;

    err_code = nrf_fstorage_read(&whitelist_storage, 0x3e000, &uuid_number, 4);
    APP_ERROR_CHECK(err_code);

    if(uuid_number == 255)
    {
        uuid_number = 0;
    }
    else
    {
        for(uint8_t i = 0; i < uuid_number; i++)
        {
            nrf_fstorage_read(&whitelist_storage, flash_addr, &whitelist[i], 16);
            flash_addr += 0x10;
        }
    } 
}


int main(void)
{
    ret_code_t rc;

    log_init();
    timer_init();
    power_init();
    uart_init();
    application_timer_init();
    buttons_init();
    leds_init();
    db_discovery_init();
    ble_stack_init();
    gatt_init();
    nus_c_init();
   
    rc = nrf_fstorage_init(&whitelist_storage, &nrf_fstorage_sd, NULL);
    APP_ERROR_CHECK(rc);

    read_flash();

    /* Start scanning for peripherals and initiate connection
       with devices that advertise NUS/EHSB UUID.*/
    NRF_LOG_INFO("EHSB Central started.");
    scan_start();


    for (;;)
    {
        if (NRF_LOG_PROCESS() == false)
        {
            nrf_pwr_mgmt_run();
        }

    }
}
