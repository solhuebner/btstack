/*
 * Copyright (C) 2024 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at
 * contact@bluekitchen-gmbh.com
 *
 */


/*
 *  btp_hap.h
 *  BTP HAP Implementation
 */

#ifndef BTP_HAP_H
#define BTP_HAP_H

#include "btstack_run_loop.h"
#include "btstack_defines.h"
#include "bluetooth.h"
#include "btp_server.h"


#include <stdint.h>

#if defined __cplusplus
extern "C" {
#endif

#ifndef BT_ADDR_LE_T
#define BT_ADDR_LE_T
typedef struct {
    uint8_t address_type;
    bd_addr_t address;
} bt_addr_le_t;
#endif

// pack structs
#pragma pack(1)
#define __packed
/* HAP commands */
#define BTP_HAP_READ_SUPPORTED_COMMANDS         0x01
struct btp_hap_read_supported_commands_rp {
	uint8_t data[0];
} __packed;

#define BTP_HAP_HA_OPT_PRESETS_SYNC             0x01
#define BTP_HAP_HA_OPT_PRESETS_INDEPENDENT      0x02
#define BTP_HAP_HA_OPT_PRESETS_DYNAMIC          0x04
#define BTP_HAP_HA_OPT_PRESETS_WRITABLE         0x08

#define BTP_HAP_HA_INIT                         0x02
struct btp_hap_ha_init_cmd {
	uint8_t type;
	uint16_t opts;
} __packed;

#define BTP_HAP_HARC_INIT                       0x03

#define BTP_HAP_HAUC_INIT                       0x04

#define BTP_HAP_IAC_INIT                        0x05

#define BTP_HAP_IAC_DISCOVER			0x06
struct btp_hap_iac_discover_cmd {
	bt_addr_le_t address;
} __packed;

#define BTP_HAP_IAC_SET_ALERT			0x07
struct btp_hap_iac_set_alert_cmd {
	bt_addr_le_t address;
	uint8_t alert;
} __packed;

#define BTP_HAP_HAUC_DISCOVER			0x08
struct btp_hap_hauc_discover_cmd {
	bt_addr_le_t address;
} __packed;

#define HAP_READ_PRESETS           0x09
#define HAP_WRITE_PRESET_NAME      0x0A
#define HAP_SET_ACTIVE_PRESET      0x0B
#define HAP_SET_NEXT_PRESET        0x0C
#define HAP_SET_PREVIOUS_PRESET    0x0D

/* HAP events */
#define BT_HAP_EV_IAC_DISCOVERY_COMPLETE        0x80
struct btp_hap_iac_discovery_complete_ev {
	bt_addr_le_t address;
	uint8_t status;
} __packed;

#define BT_HAP_EV_HAUC_DISCOVERY_COMPLETE       0x81
struct btp_hap_hauc_discovery_complete_ev {
	bt_addr_le_t address;
	uint8_t status;
	uint16_t has_hearing_aid_features_handle;
	uint16_t has_control_point_handle;
	uint16_t has_active_preset_index_handle;
} __packed;

#define HAP_EV_HAUC_PRESET_READ         0x82
#define HAP_EV_HAUC_PRESET_CHANGED      0x83

#pragma options align=reset

/**
* Init HAP Service
*/
void btp_hap_init(void);

/**
 * Process HAP Operation
 */
void btp_hap_handler(uint8_t opcode, uint8_t controller_index, uint16_t length, const uint8_t *data);

#endif