/*
 * Copyright (C) 2022 BlueKitchen GmbH
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

#define BTSTACK_FILE__ "btstack_chipset_realtek.c"

/*
 *  btstack_chipset_realtek.c
 *
 *  Adapter to use REALTEK-based chipsets with BTstack
 */

#include "btstack_chipset_realtek.h"

#include <inttypes.h>
#include <stddef.h> /* NULL */
#include <stdio.h>
#include <string.h> /* memcpy */

#include "btstack_control.h"
#include "btstack_debug.h"
#include "btstack_event.h"
#include "btstack_linked_list.h"
#include "btstack_util.h"
#include "hci.h"
#include "hci_transport.h"

#ifdef _MSC_VER
// ignore deprecated warning for fopen
#pragma warning(disable : 4996)
#endif

#define ROM_LMP_NONE 0x0000
#define ROM_LMP_8723a 0x1200
#define ROM_LMP_8723b 0x8723
#define ROM_LMP_8821a 0x8821
#define ROM_LMP_8761a 0x8761
#define ROM_LMP_8822b 0x8822
#define ROM_LMP_8852a 0x8852
#define ROM_LMP_8851b 0x8851

#define HCI_OPCODE_HCI_RTK_DOWNLOAD_FW      0xFC20
#define HCI_OPCODE_HCI_RTK_READ_ROM_VERSION 0xFC6D
#define HCI_OPCODE_HCI_RTK_8761C_COMMON		0xFDBB

#define READ_SEC_PROJ 4

#define HCI_CMD_SET_OPCODE(buf, opcode) little_endian_store_16(buf, 0, opcode)
#define HCI_CMD_SET_LENGTH(buf, length) buf[2] = length
#define HCI_CMD_DOWNLOAD_SET_INDEX(buf, index) buf[3] = index
#define HCI_CMD_DOWNLOAD_COPY_FW_DATA(buf, firmware, ptr, len) memcpy(buf + 4, firmware + ptr, len)

#define PATCH_SNIPPETS		0x01
#define PATCH_DUMMY_HEADER	0x02
#define PATCH_SECURITY_HEADER	0x03
#define PATCH_OTA_FLAG		0x04
#define SECTION_HEADER_SIZE	8

/* software id USB */
#define RTLPREVIOUS	0x00
#define RTL8822BU	0x70
#define RTL8723DU	0x71
#define RTL8821CU	0x72
#define RTL8822CU	0x73
#define RTL8761BU	0x74
#define RTL8852AU	0x75
#define RTL8723FU	0x76
#define RTL8852BU	0x77
#define RTL8852CU	0x78
#define RTL8822EU	0x79
#define RTL8851BU	0x7A

/* software id UARRT */
#define CHIP_UNKNOWN	0x00
#define CHIP_8761AT		0x1F
#define CHIP_8761ATF	0x2F
#define CHIP_8761BTC	0x3F
#define CHIP_8761BH4	0x4F
#define CHIP_8723BS	    0x5F
#define CHIP_BEFORE	    0x6F
#define CHIP_8822BS	    0x70
#define CHIP_8723DS	    0x71
#define CHIP_8821CS	    0x72
#define CHIP_8822CS	    0x73
#define CHIP_8761B	    0x74
#define CHIP_8852AS	    0x75
#define CHIP_8733BS	    0x76
#define CHIP_8852CS	    0x77
#define CHIP_8852BP	    0x78
#define CHIP_8822ES	    0x79
#define CHIP_8851BS	    0x7A
#define CHIP_8852DS	    0x7B
#define CHIP_8922AS	    0x7C
#define CHIP_8852BTS	0x7D
#define CHIP_8761CTV	0x80
#define CHIP_8723CS		0x81


#pragma pack(push, 1)
struct rtk_epatch_entry {
    uint16_t chipID;
    uint16_t patch_length;
    uint32_t start_offset;
};

struct rtk_epatch {
    uint8_t signature[8];
    uint32_t fw_version;
    uint16_t number_of_total_patch;
    struct rtk_epatch_entry entry[0];
};

struct rtk_extension_entry {
    uint8_t opcode;
    uint8_t length;
    uint8_t *data;
};

struct rtb_section_hdr {
    uint32_t opcode;
    uint32_t section_len;
    uint32_t soffset;
};

struct rtb_new_patch_hdr {
    uint8_t signature[8];
    uint8_t fw_version[8];
    uint32_t number_of_section;
};
#pragma pack(pop)

// Patch Info USB

typedef struct {
    uint16_t prod_id;
    uint16_t lmp_sub;
    char *   mp_patch_name;
    char *   patch_name;
    char *   config_name;

    uint8_t *fw_cache1;
    int      fw_len1;
    uint8_t chip_type;
} patch_info_usb;

static const patch_info_usb fw_patch_table_usb[] = {
/* { pid, lmp_sub, mp_fw_name, fw_name, config_name, chip_type } */
    {0x1724, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* RTL8723A */
    {0x8723, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* 8723AE */
    {0xA723, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* 8723AE for LI */
    {0x0723, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* 8723AE */
    {0x3394, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* 8723AE for Azurewave */

    {0x0724, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* 8723AU */
    {0x8725, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* 8723AU */
    {0x872A, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* 8723AU */
    {0x872B, 0x1200, "mp_rtl8723a_fw", "rtl8723a_fw", "rtl8723a_config", NULL, 0, RTLPREVIOUS},	/* 8723AU */

    {0xb720, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BU */
    {0xb72A, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BU */
    {0xb728, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE for LC */
    {0xb723, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE */
    {0xb72B, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE */
    {0xb001, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE for HP */
    {0xb002, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE */
    {0xb003, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE */
    {0xb004, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE */
    {0xb005, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE */

    {0x3410, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE for Azurewave */
    {0x3416, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE for Azurewave */
    {0x3459, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE for Azurewave */
    {0xE085, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE for Foxconn */
    {0xE08B, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE for Foxconn */
    {0xE09E, 0x8723, "mp_rtl8723b_fw", "rtl8723b_fw", "rtl8723b_config", NULL, 0, RTLPREVIOUS},	/* RTL8723BE for Foxconn */

    {0xA761, 0x8761, "mp_rtl8761a_fw", "rtl8761au_fw", "rtl8761a_config", NULL, 0, RTLPREVIOUS},	/* RTL8761AU only */
    {0x818B, 0x8761, "mp_rtl8761a_fw", "rtl8761aw_fw", "rtl8761aw_config", NULL, 0, RTLPREVIOUS},	/* RTL8761AW + 8192EU */
    {0x818C, 0x8761, "mp_rtl8761a_fw", "rtl8761aw_fw", "rtl8761aw_config", NULL, 0, RTLPREVIOUS},	/* RTL8761AW + 8192EU */
    {0x8760, 0x8761, "mp_rtl8761a_fw", "rtl8761au8192ee_fw", "rtl8761a_config", NULL, 0, RTLPREVIOUS},	/* RTL8761AU + 8192EE */
    {0xB761, 0x8761, "mp_rtl8761a_fw", "rtl8761au_fw", "rtl8761a_config", NULL, 0, RTLPREVIOUS},	/* RTL8761AUV only */
    {0x8761, 0x8761, "mp_rtl8761a_fw", "rtl8761au8192ee_fw", "rtl8761a_config", NULL, 0, RTLPREVIOUS},	/* RTL8761AU + 8192EE for LI */
    {0x8A60, 0x8761, "mp_rtl8761a_fw", "rtl8761au8812ae_fw", "rtl8761a_config", NULL, 0, RTLPREVIOUS},	/* RTL8761AU + 8812AE */
    {0x3527, 0x8761, "mp_rtl8761a_fw", "rtl8761au8192ee_fw", "rtl8761a_config", NULL, 0, RTLPREVIOUS},	/* RTL8761AU + 8814AE */

    {0x8821, 0x8821, "mp_rtl8821a_fw", "rtl8821a_fw", "rtl8821a_config", NULL, 0, RTLPREVIOUS},	/* RTL8821AE */
    {0x0821, 0x8821, "mp_rtl8821a_fw", "rtl8821a_fw", "rtl8821a_config", NULL, 0, RTLPREVIOUS},	/* RTL8821AE */
    {0x0823, 0x8821, "mp_rtl8821a_fw", "rtl8821a_fw", "rtl8821a_config", NULL, 0, RTLPREVIOUS},	/* RTL8821AU */
    {0x3414, 0x8821, "mp_rtl8821a_fw", "rtl8821a_fw", "rtl8821a_config", NULL, 0, RTLPREVIOUS},	/* RTL8821AE */
    {0x3458, 0x8821, "mp_rtl8821a_fw", "rtl8821a_fw", "rtl8821a_config", NULL, 0, RTLPREVIOUS},	/* RTL8821AE */
    {0x3461, 0x8821, "mp_rtl8821a_fw", "rtl8821a_fw", "rtl8821a_config", NULL, 0, RTLPREVIOUS},	/* RTL8821AE */
    {0x3462, 0x8821, "mp_rtl8821a_fw", "rtl8821a_fw", "rtl8821a_config", NULL, 0, RTLPREVIOUS},	/* RTL8821AE */

    {0xb82c, 0x8822, "mp_rtl8822bu_fw", "rtl8822bu_fw", "rtl8822bu_config", NULL, 0, RTL8822BU}, /* RTL8822BU */

    {0xd720, 0x8723, "mp_rtl8723du_fw", "rtl8723du_fw", "rtl8723du_config", NULL, 0, RTL8723DU}, /* RTL8723DU */
    {0xd723, 0x8723, "mp_rtl8723du_fw", "rtl8723du_fw", "rtl8723du_config", NULL, 0, RTL8723DU}, /* RTL8723DU */
    {0xd739, 0x8723, "mp_rtl8723du_fw", "rtl8723du_fw", "rtl8723du_config", NULL, 0, RTL8723DU}, /* RTL8723DU */
    {0xb009, 0x8723, "mp_rtl8723du_fw", "rtl8723du_fw", "rtl8723du_config", NULL, 0, RTL8723DU}, /* RTL8723DU */
    {0x0231, 0x8723, "mp_rtl8723du_fw", "rtl8723du_fw", "rtl8723du_config", NULL, 0, RTL8723DU}, /* RTL8723DU for LiteOn */

    {0xb820, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CU */
    {0xc820, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CU */
    {0xc821, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xc823, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xc824, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xc825, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xc827, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xc025, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xc024, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xc030, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xb00a, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xb00e, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0xc032, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0x4000, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for LiteOn */
    {0x4001, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for LiteOn */
    {0x3529, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3530, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3532, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3533, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3538, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3539, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3558, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3559, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3581, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for Azurewave */
    {0x3540, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE */
    {0x3541, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for GSD */
    {0x3543, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CE for GSD */
    {0xc80c, 0x8821, "mp_rtl8821cu_fw", "rtl8821cu_fw", "rtl8821cu_config", NULL, 0, RTL8821CU}, /* RTL8821CUH */

    {0xc82c, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CU */
    {0xc82e, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CU */
    {0xc81d, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CU */
    {0xd820, 0x8822, "mp_rtl8821du_fw", "rtl8821du_fw", "rtl8821du_config", NULL, 0, RTL8822CU}, /* RTL8821DU */

    {0xc822, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc82b, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xb00c, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xb00d, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc123, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc126, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc127, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc128, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc129, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc131, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc136, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0x3549, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE for Azurewave */
    {0x3548, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE for Azurewave */
    {0xc125, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0x4005, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE for LiteOn */
    {0x3051, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE for LiteOn */
    {0x18ef, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0x161f, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0x3053, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc547, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0x3553, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0x3555, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE */
    {0xc82f, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE-VS */
    {0xc02f, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE-VS */
    {0xc03f, 0x8822, "mp_rtl8822cu_fw", "rtl8822cu_fw", "rtl8822cu_config", NULL, 0, RTL8822CU}, /* RTL8822CE-VS */

    {0x8771, 0x8761, "mp_rtl8761b_fw", "rtl8761bu_fw", "rtl8761bu_config", NULL, 0, RTL8761BU}, /* RTL8761BU only */
    {0xa725, 0x8761, "mp_rtl8761b_fw", "rtl8725au_fw", "rtl8725au_config", NULL, 0, RTL8761BU}, /* RTL8725AU */
    {0xa72A, 0x8761, "mp_rtl8761b_fw", "rtl8725au_fw", "rtl8725au_config", NULL, 0, RTL8761BU}, /* RTL8725AU BT only */

    {0x885a, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AU */
    {0x8852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0xa852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x2852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x385a, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x3852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x1852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x4852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x4006, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x3561, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x3562, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x588a, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x589a, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x590a, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0xc125, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0xe852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0xb852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0xc852, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0xc549, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0xc127, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */
    {0x3565, 0x8852, "mp_rtl8852au_fw", "rtl8852au_fw", "rtl8852au_config", NULL, 0, RTL8852AU}, /* RTL8852AE */

    {0xb733, 0x8723, "mp_rtl8723fu_fw", "rtl8723fu_fw", "rtl8723fu_config", NULL, 0, RTL8723FU}, /* RTL8723FU */
    {0xb73a, 0x8723, "mp_rtl8723fu_fw", "rtl8723fu_fw", "rtl8723fu_config", NULL, 0, RTL8723FU}, /* RTL8723FU */
    {0xf72b, 0x8723, "mp_rtl8723fu_fw", "rtl8723fu_fw", "rtl8723fu_config", NULL, 0, RTL8723FU}, /* RTL8723FU */

    {0x8851, 0x8852, "mp_rtl8851au_fw", "rtl8851au_fw", "rtl8851au_config", NULL, 0, RTL8852BU}, /* RTL8851AU */
    {0xa85b, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BU */
    {0xb85b, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0xb85c, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x3571, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x3570, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x3572, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x4b06, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x885b, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x886b, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x887b, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0xc559, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0xb052, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0xb152, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0xb252, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x4853, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */
    {0x1670, 0x8852, "mp_rtl8852bu_fw", "rtl8852bu_fw", "rtl8852bu_config", NULL, 0, RTL8852BU}, /* RTL8852BE */

    {0xc85a, 0x8852, "mp_rtl8852cu_fw", "rtl8852cu_fw", "rtl8852cu_config", NULL, 0, RTL8852CU}, /* RTL8852CU */
    {0x0852, 0x8852, "mp_rtl8852cu_fw", "rtl8852cu_fw", "rtl8852cu_config", NULL, 0, RTL8852CU}, /* RTL8852CE */
    {0x5852, 0x8852, "mp_rtl8852cu_fw", "rtl8852cu_fw", "rtl8852cu_config", NULL, 0, RTL8852CU}, /* RTL8852CE */
    {0xc85c, 0x8852, "mp_rtl8852cu_fw", "rtl8852cu_fw", "rtl8852cu_config", NULL, 0, RTL8852CU}, /* RTL8852CE */
    {0x885c, 0x8852, "mp_rtl8852cu_fw", "rtl8852cu_fw", "rtl8852cu_config", NULL, 0, RTL8852CU}, /* RTL8852CE */
    {0x886c, 0x8852, "mp_rtl8852cu_fw", "rtl8852cu_fw", "rtl8852cu_config", NULL, 0, RTL8852CU}, /* RTL8852CE */
    {0x887c, 0x8852, "mp_rtl8852cu_fw", "rtl8852cu_fw", "rtl8852cu_config", NULL, 0, RTL8852CU}, /* RTL8852CE */
    {0x4007, 0x8852, "mp_rtl8852cu_fw", "rtl8852cu_fw", "rtl8852cu_config", NULL, 0, RTL8852CU}, /* RTL8852CE */

    {0xe822, 0x8822, "mp_rtl8822eu_fw", "rtl8822eu_fw", "rtl8822eu_config", NULL, 0, RTL8822EU}, /* RTL8822EU */
    {0xa82a, 0x8822, "mp_rtl8822eu_fw", "rtl8822eu_fw", "rtl8822eu_config", NULL, 0, RTL8822EU}, /* RTL8822EU */

    {0xb851, 0x8851, "mp_rtl8851bu_fw", "rtl8851bu_fw", "rtl8851bu_config", NULL, 0, RTL8851BU}, /* RTL8851BU */

/* NOTE: must append patch entries above the null entry */
    {0, 0, NULL, NULL, NULL, NULL, 0, 0}
};

// Patch Info UART

#define RTL_FW_MATCH_CHIP_TYPE  (1 << 0)
#define RTL_FW_MATCH_HCI_VER    (1 << 1)
#define RTL_FW_MATCH_HCI_REV    (1 << 2)
struct patch_info_uart {
    uint32_t    match_flags;
    uint8_t     chip_type;
    uint16_t    lmp_subver;
    uint16_t    proj_id;
    uint8_t     hci_ver;
    uint16_t    hci_rev;
    char        *patch_file;
    char        *config_file;
    char        *ic_name;
};


static struct patch_info_uart h4_patch_table[] = {
    /* match flags, chip type, lmp subver, proj id(unused), hci_ver,
     * hci_rev, ...
     */

    /* RTL8761AT */
    { RTL_FW_MATCH_CHIP_TYPE, CHIP_8761AT,
        0x8761, 0xffff, 0, 0x000a,
        "rtl8761at_fw", "rtl8761at_config", "RTL8761AT" },
    /* RTL8761ATF */
    { RTL_FW_MATCH_CHIP_TYPE, CHIP_8761ATF,
        0x8761, 0xffff, 0, 0x000a,
        "rtl8761atf_fw", "rtl8761atf_config", "RTL8761ATF" },
    /* RTL8761B(8763) H4 Test Chip without download
     * FW/Config is not used.
     */
    { RTL_FW_MATCH_CHIP_TYPE, CHIP_8761BTC,
        0x8763, 0xffff, 0, 0x000b,
        "rtl8761btc_fw", "rtl8761btc_config", "RTL8761BTC" },
    /* RTL8761B H4 Test Chip wihtout download*/
    { RTL_FW_MATCH_CHIP_TYPE, CHIP_8761BH4,
        0x8761, 0xffff, 0, 0x000b,
        "rtl8761bh4_fw", "rtl8761bh4_config", "RTL8761BH4" },

    /* RTL8723DS */
    { RTL_FW_MATCH_HCI_VER | RTL_FW_MATCH_HCI_REV, CHIP_8723DS,
        ROM_LMP_8723b, ROM_LMP_8723b, 8, 0x000d,
        "rtl8723dsh4_fw", "rtl8723dsh4_config", "RTL8723DSH4"},
    /* RTL8761C */
    { RTL_FW_MATCH_HCI_REV, CHIP_8761CTV,
        ROM_LMP_8761a, 0xffff, 0x0c, 0x000e,
        "rtl8761c_mx_fw", "rtl8761c_mx_config", "RTL8761CTV" },

    { 0, 0, 0, ROM_LMP_NONE, 0, 0, "rtl_none_fw", "rtl_none_config", "NONE"}
};


static uint16_t project_id[] = {
    ROM_LMP_8723a, ROM_LMP_8723b, ROM_LMP_8821a, ROM_LMP_8761a, ROM_LMP_NONE,
    ROM_LMP_NONE,  ROM_LMP_NONE,  ROM_LMP_NONE,  ROM_LMP_8822b, ROM_LMP_8723b, /* RTL8723DU */
    ROM_LMP_8821a,                                                             /* RTL8821CU */
    ROM_LMP_NONE,  ROM_LMP_NONE,  ROM_LMP_8822b,                               /* RTL8822CU */
    ROM_LMP_8761a,                                                             /* index 14 for 8761BU */
    ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_8852a,                   /* index 18 for 8852AU */
    ROM_LMP_8723b,                                                             /* index 19 for 8723FU */
    ROM_LMP_8852a,                                                             /* index 20 for 8852BU */
    ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_8852a,     /* index 25 for 8852CU */
    ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE,
    ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_8822b,                                 /* index 33 for 8822EU */
    ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_8851b,                                 /* index 36 for 8851BU */
    ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE,      // 37~41
    ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE,      // 42~46
    ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_NONE, ROM_LMP_8761a,     // index 51 for 8761CTV
};

static const uint8_t FW_SIGNATURE[8]        = {0x52, 0x65, 0x61, 0x6C, 0x74, 0x65, 0x63, 0x68};
static const uint8_t FW_SIGNATURE_NEW[8]    = {0x52, 0x54, 0x42, 0x54, 0x43, 0x6F, 0x72, 0x65};
static const uint8_t EXTENSION_SIGNATURE[4] = {0x51, 0x04, 0xFD, 0x77};

typedef enum {
    REALTEK_INTERFACE_UNKNOWN = 0,
    REALTEK_INTERFACE_USB,
    REALTEK_INTERFACE_UART_H4,
    REALTEK_INTERFACE_UART_H5,
} realtek_interface_t;

typedef struct rtb_struct {
    uint16_t hci_version;
    uint16_t hci_revision;
    uint16_t lmp_subversion;
    uint8_t  chip_type;
    uint32_t vendor_baud;
} rtb_struct_t;

enum {
    // Pre-Init: runs before HCI Reset
    STATE_PHASE_1_READ_LMP_SUBVERSION,
    STATE_PHASE_1_W4_READ_LMP_SUBVERSION,
    STATE_PHASE_1_READ_HCI_REVISION,
    STATE_PHASE_1_W4_READ_HCI_REVISION,
    STATE_PHASE_1_DONE,
    // Custom Init: runs after HCI Reset
    STATE_PHASE_2_READ_ROM_VERSION,
    STATE_PHASE_2_READ_SEC_PROJ,
    STATE_PHASE_2_W4_SEC_PROJ,
    STATE_PHASE_2_LOAD_FIRMWARE,
    STATE_PHASE_2_CHECK_UPGRADE,
    STATE_PHASE_2_RESET,
    STATE_PHASE_2_DONE,
};

static realtek_interface_t realtek_interface = REALTEK_INTERFACE_UNKNOWN;

static btstack_packet_callback_registration_t hci_event_callback_registration;
static uint8_t                                state;
static uint16_t                               product_id;
static uint8_t                                rom_version;
static const patch_info_usb *                 patch_usb;
static const struct patch_info_uart *         patch_uart;
static uint8_t                                g_key_id = 0;
static rtb_struct_t                           rtb_cfg;

#ifdef HAVE_POSIX_FILE_IO
static const char *firmware_folder_path = ".";
static const char *firmware_file_path;
static const char *config_folder_path = ".";
static const char *config_file_path;
static char        firmware_file[1000];
static char        config_file[1000];
#endif

enum { FW_DONE, FW_MORE_TO_DO };

#ifdef HAVE_POSIX_FILE_IO

/**
 * @brief Opens the specified file and stores content to an allocated buffer
 *
 * @param file
 * @param buf
 * @param name
 * @return uint32_t Length of file
 */
static uint32_t read_file(FILE **file, uint8_t **buf, const char *name) {
    uint32_t size;

    // open file
    *file = fopen(name, "rb");
    if (*file == NULL) {
        log_info("Failed to open file %s", name);
        return 0;
    }

    // determine length of file
    fseek(*file, 0, SEEK_END);
    size = ftell(*file);
    fseek(*file, 0, SEEK_SET);
    if (size <= 0) {
        return 0;
    }

    // allocate memory
    *buf = malloc(size);
    if (*buf == NULL) {
        fclose(*file);
        *file = NULL;
        log_info("Failed to allocate %u bytes for file %s", size, name);
        return 0;
    }

    // read file
    size_t ret = fread(*buf, size, 1, *file);
    if (ret != 1) {
        log_info("Failed to read %u bytes from file %s (ret = %d)", size, name, (int) ret);
        fclose(*file);
        free(*buf);
        *file = NULL;
        *buf  = NULL;
        return 0;
    }

    log_info("Opened file %s and read %u bytes", name, size);
    return size;
}

static void finalize_file_and_buffer(FILE **file, uint8_t **buffer) {
    fclose(*file);
    free(*buffer);
    *buffer = NULL;
    *file   = NULL;
}

static uint8_t rtk_get_fw_project_id(uint8_t * p_buf)
{
    uint8_t opcode;
    uint8_t len;
    uint8_t data = 0;

    do {
        opcode = *p_buf;
        len = *(p_buf - 1);
        if (opcode == 0x00) {
            if (len == 1) {
                data = *(p_buf - 2);
                log_info
                    ("rtk_get_fw_project_id: opcode %d, len %d, data %d",
                     opcode, len, data);
                break;
            } else {
                log_error
                    ("rtk_get_fw_project_id: invalid len %d",
                     len);
            }
        }
        p_buf -= len + 2;
    } while (*p_buf != 0xFF);

    return data;
}

struct rtb_ota_flag {
    uint8_t eco;
    uint8_t enable;
    uint16_t reserve;
};

struct patch_node {
    btstack_linked_item_t item;
    uint8_t eco;
    uint8_t pri;
    uint8_t key_id;
    uint8_t reserve;
    uint32_t len;
    uint8_t *payload;
};

/* Add a node to alist that is in ascending order. */
static void insert_queue_sort(btstack_linked_list_t * list, struct patch_node *node)
{
    btstack_assert(list != NULL);
    btstack_assert(node != NULL);

    struct patch_node *next;
    btstack_linked_item_t *it;

    for (it = (btstack_linked_item_t *) list; it->next ; it = it->next){
        next = (struct patch_node *) it->next;
        if(next->pri >= node->pri) {
            break;
        }
    }
    node->item.next = it->next;
    it->next = (btstack_linked_item_t *) node;
}

static int insert_patch(btstack_linked_list_t * patch_list, uint8_t *section_pos,
        uint32_t opcode, uint32_t *patch_len, uint8_t *sec_flag)
{
    struct patch_node *tmp;
    uint32_t i;
    uint32_t numbers;
    uint32_t section_len = 0;
    uint8_t eco = 0;
    uint8_t *pos = section_pos + 8;

    numbers = little_endian_read_16(pos, 0);
    log_info("number 0x%04x", numbers);

    pos += 4;
    for (i = 0; i < numbers; i++) {
        eco = (uint8_t)*(pos);
        log_info("eco 0x%02x, Eversion:%02x", eco, rom_version);
        if (eco == rom_version + 1) {
            //tmp = (struct patch_node*)kzalloc(sizeof(struct patch_node), GFP_KERNEL);
            tmp = (struct patch_node*)malloc(sizeof(struct patch_node));
            tmp->pri = (uint8_t)*(pos + 1);
            if(opcode == PATCH_SECURITY_HEADER)
                tmp->key_id = (uint8_t)*(pos + 1);

            section_len = little_endian_read_32(pos, 4);
            tmp->len =  section_len;
            *patch_len += section_len;
            log_info("Pri:%d, Patch length 0x%04x", tmp->pri, tmp->len);
            tmp->payload = pos + 8;
            if(opcode != PATCH_SECURITY_HEADER) {
                insert_queue_sort(patch_list, tmp);
            } else {
                if((g_key_id == tmp->key_id) && (g_key_id > 0)) {
                    insert_queue_sort(patch_list, tmp);
                    *sec_flag = 1;
                } else {
                    pos += (8 + section_len);
                    free(tmp);
                    continue;
                }
            }
        } else {
            section_len =  little_endian_read_32(pos, 4);
            log_info("Patch length 0x%04x", section_len);
        }
        pos += (8 + section_len);
    }
    return 0;
}
static uint8_t *rtb_get_patch_header(uint32_t *len,
                                     btstack_linked_list_t * patch_list, uint8_t * epatch_buf,
                                     uint8_t key_id)
{
    UNUSED(key_id);
    uint16_t i, j;
    struct rtb_new_patch_hdr *new_patch;
    uint8_t sec_flag = 0;
    uint32_t number_of_ota_flag;
    uint32_t patch_len = 0;
    uint8_t *section_pos;
    uint8_t *ota_flag_pos;
    uint32_t number_of_section;

    struct rtb_section_hdr section_hdr;
    struct rtb_ota_flag ota_flag;

    new_patch = (struct rtb_new_patch_hdr *)epatch_buf;
    number_of_section = new_patch->number_of_section;

    log_info("FW version 0x%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x",
                *(epatch_buf + 8), *(epatch_buf + 9), *(epatch_buf + 10),
                *(epatch_buf + 11),*(epatch_buf + 12), *(epatch_buf + 13),
                *(epatch_buf + 14), *(epatch_buf + 15));

    section_pos = epatch_buf + 20;

    for (i = 0; i < number_of_section; i++) {
        section_hdr.opcode = little_endian_read_32(section_pos, 0);
        section_hdr.section_len = little_endian_read_32(section_pos, 4);
        log_info("opcode 0x%04x", section_hdr.opcode);
        switch (section_hdr.opcode) {
        case PATCH_SNIPPETS:
            insert_patch(patch_list, section_pos, PATCH_SNIPPETS, &patch_len, NULL);
            log_info("Realtek: patch len is %d",patch_len);
            break;
        case PATCH_SECURITY_HEADER:
            if(!g_key_id)
                break;

            sec_flag = 0;
            insert_patch(patch_list, section_pos, PATCH_SECURITY_HEADER, &patch_len, &sec_flag);
            if(sec_flag)
                break;

            for (i = 0; i < number_of_section; i++) {
                section_hdr.opcode = little_endian_read_32(section_pos, 0);
                section_hdr.section_len = little_endian_read_32(section_pos, 4);
                if(section_hdr.opcode == PATCH_DUMMY_HEADER) {
                    insert_patch(patch_list, section_pos, PATCH_DUMMY_HEADER, &patch_len, NULL);
                }
                section_pos += (SECTION_HEADER_SIZE + section_hdr.section_len);
            }
            break;
        case PATCH_DUMMY_HEADER:
            if(g_key_id) {
                break;
            }
            insert_patch(patch_list, section_pos, PATCH_DUMMY_HEADER, &patch_len, NULL);
            break;
        case PATCH_OTA_FLAG:
            ota_flag_pos = section_pos + 4;
            number_of_ota_flag = little_endian_read_32(ota_flag_pos, 0);
            ota_flag.eco = (uint8_t)*(ota_flag_pos + 1);
            if (ota_flag.eco == rom_version + 1) {
                for (j = 0; j < number_of_ota_flag; j++) {
                    if (ota_flag.eco == rom_version + 1) {
                        ota_flag.enable = little_endian_read_32(ota_flag_pos, 4);
                    }
                }
            }
            break;
        default:
            log_error("Unknown Opcode");
            break;
        }
        section_pos += (SECTION_HEADER_SIZE + section_hdr.section_len);
    }
    *len = patch_len;

    return NULL;
}

static struct patch_info_uart *get_patch_entry(struct rtb_struct *btrtl)
{
    struct patch_info_uart *n = NULL;

    switch (realtek_interface) {
        case REALTEK_INTERFACE_UART_H4:
            n = h4_patch_table;
            break;
        default:
            printf("Realtek: Unknown interface type %d\n", realtek_interface);
            btstack_assert(false);
            return NULL;
    }
    for (; n->lmp_subver; n++) {
        if ((n->match_flags & RTL_FW_MATCH_CHIP_TYPE) &&
            n->chip_type != btrtl->chip_type)
            continue;
        if ((n->match_flags & RTL_FW_MATCH_HCI_VER) &&
            n->hci_ver != btrtl->hci_version)
            continue;
        if ((n->match_flags & RTL_FW_MATCH_HCI_REV) &&
            n->hci_rev != btrtl->hci_revision)
            continue;
        if (n->lmp_subver != btrtl->lmp_subversion)
            continue;

        break;
    }

    return n;
}

static inline int get_max_patch_size(uint8_t chip_type)
{
    int max_patch_size = 0;

    switch (chip_type) {
    case RTLPREVIOUS:
        max_patch_size = 24 * 1024;
        break;
    case RTL8822BU: // CHIP_8822BS
        max_patch_size = 25 * 1024;
        break;
    case RTL8723DU: // CHIP_8723DS
    case RTL8822CU: // CHIP_8822CS
    case RTL8761BU: // CHIP_8761B
    case RTL8821CU: // CHIP_8821CS
        max_patch_size = 40 * 1024;
        break;
    case RTL8852AU: // CHIP_8852AS
        max_patch_size = 0x114D0 + 529; /* 69.2KB */
        break;
    case RTL8723FU: // CHIP_8733BS
        max_patch_size = 0xC4Cf + 529; /* 49.2KB */
        break;
    case RTL8852BU: // CHIP_8852BS
    case RTL8851BU: // CHIP_8852BP, CHIP_8851BS
        max_patch_size = 0x104D0 + 529; /* 65KB */
        break;
    case RTL8852CU: // CHIP_8852CS
        max_patch_size = 0x130D0 + 529; /* 76.2KB */
        break;
    case RTL8822EU: // CHIP_8822ES
        max_patch_size = 0x24620 + 529; /* 145KB */
        break;
    case CHIP_8852DS:
        max_patch_size = 0x20D90 + 529;  /* 131KB */
        break;
    case CHIP_8922AS:
        max_patch_size = 0x23810 + 529;  /* 142KB */
        break;
    case CHIP_8852BTS:
        max_patch_size = 0x27E00 + 529; /* 159.5KB */
        break;
    case CHIP_8761CTV:
        max_patch_size = 1024 * 1024;
        break;
    default:
        max_patch_size = 40 * 1024;
        break;
    }

    return max_patch_size;
}

#define RTB_CFG_HDR_LEN		6
static uint32_t get_vendor_baud(const uint8_t * conf_buffer, uint32_t conf_size) {
    // check header
    if (conf_size <= RTB_CFG_HDR_LEN) {
        return 0;
    }
    uint32_t magic = little_endian_read_32(conf_buffer, 0);
    if (magic != 0x8723ab55) {
        return 0;
    }
    uint16_t cfg_len = little_endian_read_16(conf_buffer, 4);
    if (cfg_len != (conf_size - RTB_CFG_HDR_LEN)) {
        return 0;
    }
    conf_buffer += RTB_CFG_HDR_LEN;
    conf_size -= RTB_CFG_HDR_LEN;

    // iterate over config sections
    while (conf_size > 0) {
        uint16_t offset = little_endian_read_16(conf_buffer, 0);
        uint8_t  len = conf_buffer[2];
        conf_buffer += 3;
        conf_size -= 3;
        if (conf_size < len) {
            return 0;
        }
        log_info("Config %04x: ", offset);
        log_info_hexdump(conf_buffer, len);
        switch (offset) {
            case 0x003c:
            case 0x0030:
            case 0x0044:
                printf("Realtek: BD ADDR in config file not supported yet");
                break;
            case 0x000c:
                return little_endian_read_32(conf_buffer, 0);
            default:
                break;
        }
        conf_buffer += len;
        conf_size -= len;
    }
    return 0;
}

struct rtb_baud {
    uint32_t rtb_speed;
    uint32_t uart_speed;
};
struct rtb_baud baudrates[] = {
#ifdef RTL_8703A_SUPPORT
    {0x00004003, 1500000}, /* for rtl8703as */
#endif
    {0x0252C014, 115200},
    {0x0252C00A, 230400},
    {0x05F75004, 921600},
    {0x00005004, 1000000},
    {0x04928002, 1500000},
    {0x01128002, 1500000},	//8761AT
    {0x00005002, 2000000},
    {0x0000B001, 2500000},
    {0x04928001, 3000000},
    {0x052A6001, 3500000},
    {0x00005001, 4000000},
};

static uint32_t vendor_speed_to_std(uint32_t rtb_speed){
    unsigned int i;
    for (i = 0; i < (sizeof(baudrates)/sizeof(baudrates[0])); i++) {
        if (baudrates[i].rtb_speed == rtb_speed) {
            return baudrates[i].uart_speed;
        }
    }
    return 0;
}

static const uint8_t hci_realtek_read_sec_proj[]       = {0x61, 0xfc, 0x05, 0x10, 0xA4, 0x0D, 0x00, 0xb0 };
static const uint8_t hci_realtek_read_lmp_subversion[] = {0x61, 0xfc, 0x05, 0x10, 0x38, 0x04, 0x28, 0x80 };
static const uint8_t hci_realtek_read_hci_revision[]   = {0x61, 0xfc, 0x05, 0x10, 0x3A, 0x04, 0x28, 0x80 };


static uint8_t * patch_buffer = NULL;
static uint32_t  config_size;
static uint32_t  firmware_size;

typedef struct {
    const uint8_t * buffer;
    uint32_t        offset;
    uint32_t        len;
    uint8_t         index;
} download_blob_t;
static download_blob_t download_blob;

// returns true if successful
static bool load_firmware_and_config(const char *firmware, const char *config) {
    btstack_assert(patch_buffer == NULL);
    uint32_t offset = 0;
    FILE *   fw = NULL;
    uint8_t *fw_buf = NULL;

    FILE *   conf = NULL;
    uint8_t *conf_buf = NULL;

    uint32_t fw_version = 0;

    struct patch_node *tmp;
    unsigned max_patch_size = 0;

    if (rtb_cfg.lmp_subversion == ROM_LMP_8723a) {
        log_info("Realtek firmware upload for old patch style not implemented");
        return false;
    }

    if (firmware == NULL || config == NULL) {
        log_info("Please specify Realtek firmware and config file paths");
        return false;
    }
    // read config
    config_size = read_file(&conf, &conf_buf, config);
    if (config_size == 0) {
        log_info("Config size is 0, using efuse settings!");
    }
    // get vendor baud
    if (conf_buf) {
        rtb_cfg.vendor_baud = get_vendor_baud(conf_buf, config_size);
        log_info("Realtek: Vendor baud from config file: %08" PRIx32, rtb_cfg.vendor_baud);
    }
    // read firmware
    uint32_t fw_size = read_file(&fw, &fw_buf, firmware);
    if (fw_size == 0) {
        log_info("Firmware size is 0. Quit!");
        if (config_size != 0){
            finalize_file_and_buffer(&conf, &conf_buf);
        }
        return false;
    }
    // check signature
    if (((memcmp(fw_buf, FW_SIGNATURE, 8) != 0) && (memcmp(fw_buf, FW_SIGNATURE_NEW, 8) != 0))
          || memcmp(fw_buf + fw_size - 4, EXTENSION_SIGNATURE, 4) != 0) {
        log_info("Wrong signature. Quit!");
        finalize_file_and_buffer(&fw, &fw_buf);
        finalize_file_and_buffer(&conf, &conf_buf);
        return false;
    }
    // check project id
    if (rtb_cfg.lmp_subversion != project_id[rtk_get_fw_project_id(fw_buf + fw_size - 5)]) {
        log_info("Wrong project id. Quit!");
        finalize_file_and_buffer(&fw, &fw_buf);
        finalize_file_and_buffer(&conf, &conf_buf);
        return false;
    }
    // init ordered list for new firmware signature
    btstack_linked_list_t patch_list = NULL;
    bool have_new_firmware_signature = memcmp(fw_buf, FW_SIGNATURE_NEW, 8) == 0;
    if (have_new_firmware_signature){
        log_info("Realtek: Using new signature");
        uint8_t key_id = g_key_id;

        // TODO: figure out how this should work for UART
        if ((realtek_interface == REALTEK_INTERFACE_USB) && (key_id == 0)) {
            log_info("Wrong key id. Quit!");
            finalize_file_and_buffer(&fw, &fw_buf);
            finalize_file_and_buffer(&conf, &conf_buf);
            return false;
        }
        rtb_get_patch_header(&firmware_size, &patch_list, fw_buf, key_id);
        if (firmware_size == 0) {
            finalize_file_and_buffer(&fw, &fw_buf);
            finalize_file_and_buffer(&conf, &conf_buf);
            return false;
        }
    } else {
        uint16_t fw_num_patches;
        log_info("Realtek: Using old signature");
        // read firmware version
        fw_version = little_endian_read_32(fw_buf, 8);
        log_info("Firmware version: 0x%x", fw_version);

        // read number of patches
        fw_num_patches = little_endian_read_16(fw_buf, 12);
        log_info("Number of patches: %d", fw_num_patches);

        // find correct entry
        for (uint16_t i = 0; i < fw_num_patches; i++) {
            if (little_endian_read_16(fw_buf, 14 + 2 * i) == rom_version + 1) {
                firmware_size = little_endian_read_16(fw_buf, 14 + 2 * fw_num_patches + 2 * i);
                offset        = little_endian_read_32(fw_buf, 14 + 4 * fw_num_patches + 4 * i);
                log_info("patch_length %" PRIu32 ", offset %u", firmware_size, offset);
                break;
            }
        }
        if (firmware_size == 0) {
            log_debug("Failed to find valid patch");
            finalize_file_and_buffer(&fw, &fw_buf);
            finalize_file_and_buffer(&conf, &conf_buf);
            return false;
        }
    }

    uint32_t download_total_len = firmware_size + config_size;

    max_patch_size = get_max_patch_size(rtb_cfg.chip_type);
    printf("Realtek: FW/CONFIG total length is %d, max patch size id %d\n", download_total_len, max_patch_size);
    if (download_total_len > max_patch_size) {
        printf("Realtek: FW/CONFIG total length larger than allowed %d\n", max_patch_size);
        finalize_file_and_buffer(&fw, &fw_buf);
        finalize_file_and_buffer(&conf, &conf_buf);
        return false;
    }
    // allocate patch buffer
    patch_buffer = malloc(download_total_len);
    if (patch_buffer == NULL) {
        log_debug("Failed to allocate %u bytes for patch buffer", fw_total_len);
        finalize_file_and_buffer(&fw, &fw_buf);
        finalize_file_and_buffer(&conf, &conf_buf);
        return false;
    }
    if (have_new_firmware_signature) {
        int tmp_len = 0;
        // append patches based on priority and free
        while (patch_list) {
            tmp = (struct patch_node *) patch_list;
            log_info("len = 0x%x", tmp->len);
            memcpy(patch_buffer + tmp_len, tmp->payload, tmp->len);
            tmp_len += tmp->len;
            patch_list = patch_list->next;
            free(tmp);
        }
        if (config_size) {
            memcpy(&patch_buffer[download_total_len - config_size], conf_buf, config_size);
        }
    } else {
        // copy patch
        memcpy(patch_buffer, fw_buf + offset, firmware_size);
        memcpy(patch_buffer + firmware_size - 4, &fw_version, 4);
        memcpy(patch_buffer + firmware_size, conf_buf, config_size);
    }

    // close files
    finalize_file_and_buffer(&fw, &fw_buf);
    finalize_file_and_buffer(&conf, &conf_buf);
    return true;
}

static uint8_t download_blob_next_command(uint8_t *hci_cmd_buffer, download_blob_t * download_blob) {

    uint8_t len;
    if ((download_blob->len - download_blob->offset) > 252) {
        len = 252;
    } else {
        len = download_blob->len - download_blob->offset;
        download_blob->index |= 0x80;  // end
    }

    if (len) {
        little_endian_store_16(hci_cmd_buffer, 0, HCI_OPCODE_HCI_RTK_DOWNLOAD_FW);
        HCI_CMD_SET_LENGTH(hci_cmd_buffer, len + 1);
        HCI_CMD_DOWNLOAD_SET_INDEX(hci_cmd_buffer, download_blob->index);
        HCI_CMD_DOWNLOAD_COPY_FW_DATA(hci_cmd_buffer, download_blob->buffer, download_blob->offset, len);
        download_blob->index++;
        if (download_blob->index > 0x7f) {
            download_blob->index = (download_blob->index & 0x7f) +1;
        }
        download_blob->offset += len;
        return FW_MORE_TO_DO;
    }

    return FW_DONE;
}

#endif  // HAVE_POSIX_FILE_IO

#define UPG_DL_BLOCK_SIZE   128
#define SUBOPCODE_CKUPG	0x01

#define DUMMY_DL_PDU_LEN	16
static uint8_t dummy_dl_pdu[DUMMY_DL_PDU_LEN] = { 0 };

static void check_upgrade_command(uint8_t *hci_cmd_buffer, const download_blob_t * download_blob) {
    const uint8_t len = (uint8_t) btstack_min(UPG_DL_BLOCK_SIZE, download_blob->len);
    little_endian_store_16(hci_cmd_buffer, 0, HCI_OPCODE_HCI_RTK_8761C_COMMON);
    hci_cmd_buffer[2] = len + 1;
    hci_cmd_buffer[3] = SUBOPCODE_CKUPG;
    memcpy(&hci_cmd_buffer[4], download_blob->buffer, len);
}

static void configure_download_blob(download_blob_t * download_blob, const uint8_t * buffer, uint32_t len) {
    download_blob->buffer = buffer;
    download_blob->len    = len;
    download_blob->offset = 0;
    download_blob->index  = 0;
}

static void chipset_prepare_download(void) {
#ifdef HAVE_POSIX_FILE_IO
    // read firmware and config
    bool ok = load_firmware_and_config(firmware_file_path, config_file_path);
    if (ok) {
        // prepare download
        if (rtb_cfg.chip_type == CHIP_8761CTV) {
            // for 8761CTV, we load the config first and use a dummy pdu if there's no config
            if (config_size) {
                configure_download_blob(&download_blob, &patch_buffer[firmware_size], config_size);
            } else {
                configure_download_blob(&download_blob, dummy_dl_pdu, DUMMY_DL_PDU_LEN);
            }
        } else {
            // otherwise, we load the firmware and config as a single blob
            configure_download_blob(&download_blob, patch_buffer, firmware_size + config_size);
        }
        state = STATE_PHASE_2_LOAD_FIRMWARE;
    } else
#endif
    {
        state = STATE_PHASE_2_RESET;
    }
}

static void chipset_upgrade_done(void) {
    // cleanup
    free(patch_buffer);
    patch_buffer = NULL;

    state = STATE_PHASE_2_RESET;
}

static btstack_chipset_result_t chipset_next_command(uint8_t *hci_cmd_buffer) {
#ifdef HAVE_POSIX_FILE_IO
    uint8_t ret;
    while (true) {
        switch (state) {
            case STATE_PHASE_1_READ_LMP_SUBVERSION:
                memcpy(hci_cmd_buffer, hci_realtek_read_lmp_subversion, sizeof(hci_realtek_read_lmp_subversion));
                state = STATE_PHASE_1_W4_READ_LMP_SUBVERSION;
                break;
            case STATE_PHASE_1_READ_HCI_REVISION:
                memcpy(hci_cmd_buffer, hci_realtek_read_hci_revision, sizeof(hci_realtek_read_hci_revision));
                state = STATE_PHASE_1_W4_READ_HCI_REVISION;
                break;
            case STATE_PHASE_1_DONE:
                // custom pre-init done, continue with read ROM version in main custom init
                state = STATE_PHASE_2_READ_ROM_VERSION;
                return BTSTACK_CHIPSET_DONE;
            case STATE_PHASE_2_READ_ROM_VERSION:
                HCI_CMD_SET_OPCODE(hci_cmd_buffer, HCI_OPCODE_HCI_RTK_READ_ROM_VERSION);
                HCI_CMD_SET_LENGTH(hci_cmd_buffer, 0);
                state = STATE_PHASE_2_READ_SEC_PROJ;
                break;
            case STATE_PHASE_2_READ_SEC_PROJ:
                memcpy(hci_cmd_buffer, hci_realtek_read_sec_proj, sizeof(hci_realtek_read_sec_proj));
                state = STATE_PHASE_2_W4_SEC_PROJ;
                break;
            case STATE_PHASE_2_LOAD_FIRMWARE:
                ret = download_blob_next_command(hci_cmd_buffer, &download_blob);
                // all commands sent?
                if (ret != FW_DONE) {
                    break;
                }
                // download blob complete
                if ( rtb_cfg.chip_type == CHIP_8761CTV) {
                    configure_download_blob(&download_blob, patch_buffer, firmware_size);
                    check_upgrade_command(hci_cmd_buffer, &download_blob);
                    state = STATE_PHASE_2_CHECK_UPGRADE;
                    break;
                }

                chipset_upgrade_done();

                /* fall through */

            case STATE_PHASE_2_RESET:
                HCI_CMD_SET_OPCODE(hci_cmd_buffer, HCI_OPCODE_HCI_RESET);
                HCI_CMD_SET_LENGTH(hci_cmd_buffer, 0);
                state = STATE_PHASE_2_DONE;
                break;
            case STATE_PHASE_2_DONE:
                hci_remove_event_handler(&hci_event_callback_registration);
                return BTSTACK_CHIPSET_DONE;
            default:
                log_info("Invalid state %d", state);
                return BTSTACK_CHIPSET_DONE;
        }
        return BTSTACK_CHIPSET_VALID_COMMAND;
    }
#else   // HAVE_POSIX_FILE_IO
    log_info("Realtek without File IO is not implemented yet");
    return BTSTACK_CHIPSET_NO_INIT_SCRIPT;
#endif  // HAVE_POSIX_FILE_IO
}

static void hci_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size) {
    UNUSED(channel);
    UNUSED(size);
    if (packet_type != HCI_EVENT_PACKET) {
        return;
    }
    if (hci_event_packet_get_type(packet) != HCI_EVENT_COMMAND_COMPLETE) {
        return;
    }

    uint16_t opcode = hci_event_command_complete_get_command_opcode(packet);
    const uint8_t * return_params = hci_event_command_complete_get_return_parameters(packet);
    switch (opcode) {
        case HCI_OPCODE_HCI_READ_LOCAL_VERSION_INFORMATION:
            rtb_cfg.lmp_subversion = little_endian_read_16(packet, 12);
            break;
        case HCI_OPCODE_HCI_RTK_READ_ROM_VERSION:
            rom_version = return_params[1];
            log_info("Received ROM version 0x%02x", rom_version);
            if (patch_usb->lmp_sub != rtb_cfg.lmp_subversion) {
                printf("Realtek: Firmware already exists\n");
                state = STATE_PHASE_2_DONE;
            }
            break;
        case HCI_OPCODE_HCI_RTK_READ_CARD_INFO:
            switch (state){
                case STATE_PHASE_1_W4_READ_LMP_SUBVERSION:
                    log_info("Read Card: LMP Subversion");
                    if (little_endian_read_16(hci_event_command_complete_get_return_parameters(packet), 1) == 0x8822){
                        state = STATE_PHASE_1_READ_HCI_REVISION;
                    } else {
                        state = STATE_PHASE_1_DONE;
                    }
                    break;
                case STATE_PHASE_1_W4_READ_HCI_REVISION:
                    log_info("Read Card: HCI Revision");
                    if (little_endian_read_16(hci_event_command_complete_get_return_parameters(packet), 1) == 0x000e){
                        state = STATE_PHASE_2_READ_ROM_VERSION;
                    } else {
                        state = STATE_PHASE_1_DONE;
                    }
                    break;
                case STATE_PHASE_2_W4_SEC_PROJ:
                    g_key_id = return_params[1];
                    printf("Realtek: Received key id 0x%02x\n", g_key_id);
                    chipset_prepare_download();
                    break;
                default:
                    btstack_assert(false);
                    break;
            }
            break;
        case HCI_OPCODE_HCI_RTK_8761C_COMMON:
            switch (state) {
                case STATE_PHASE_2_CHECK_UPGRADE:
                    btstack_assert(return_params[0] == ERROR_CODE_SUCCESS);
                    btstack_assert(return_params[1] == SUBOPCODE_CKUPG);
                    if (return_params[2] == 0x00) {
                        printf("Realtek: Upgrade in place\n");
                        chipset_upgrade_done();
                    } else {
                        printf("Realtek: Need firmware upgrade\n");
                        state = STATE_PHASE_2_LOAD_FIRMWARE;
                    }
                    break;
                default:
                    btstack_unreachable();
                    break;
            }
            break;
        default:
            break;
    }
}

static void chipset_init(const void *config) {
    UNUSED(config);

#ifdef HAVE_POSIX_FILE_IO
    // default: no init, just clean up
    state = STATE_PHASE_2_DONE;

    // lookup chipset by USB Product ID
    if (product_id != 0) {
        log_info("firmware or config file path is empty. Using product id 0x%04x!", product_id);
        patch_usb = NULL;
        for (uint16_t i = 0; i < sizeof(fw_patch_table_usb) / sizeof(patch_info_usb); i++) {
            if (fw_patch_table_usb[i].prod_id == product_id) {
                patch_usb = &fw_patch_table_usb[i];
                break;
            }
        }
        if (patch_usb == NULL) {
            log_info("Product id 0x%04x is unknown", product_id);
            state = STATE_PHASE_2_DONE;
            return;
        }
        btstack_snprintf_assert_complete(firmware_file, sizeof(firmware_file), "%s/%s", firmware_folder_path, patch_usb->patch_name);
        btstack_snprintf_assert_complete(config_file, sizeof(config_file), "%s/%s", config_folder_path, patch_usb->config_name);
        firmware_file_path = &firmware_file[0];
        config_file_path   = &config_file[0];
        rtb_cfg.lmp_subversion = patch_usb->lmp_sub;
        rtb_cfg.chip_type      = patch_usb->chip_type;
        state = STATE_PHASE_1_READ_LMP_SUBVERSION;
    }

    // start lookup by local version info
    else if (rtb_cfg.lmp_subversion != 0) {
        patch_uart = get_patch_entry(&rtb_cfg);
        if (patch_uart == NULL) {
            log_info("Cannot find chipset for hci/lmp info");
            state = STATE_PHASE_2_DONE;
            return;
        }
        btstack_snprintf_assert_complete(firmware_file, sizeof(firmware_file), "%s/%s", firmware_folder_path, patch_uart->patch_file);
        btstack_snprintf_assert_complete(config_file, sizeof(config_file), "%s/%s", config_folder_path, patch_uart->config_file);
        firmware_file_path = &firmware_file[0];
        config_file_path   = &config_file[0];
        rtb_cfg.chip_type = patch_uart->chip_type;
        printf("Realtek: IC: %s, chip type: 0x%02x\n", patch_uart->ic_name, patch_uart->chip_type);
        chipset_prepare_download();
    }

    log_info("Using firmware '%s' and config '%s'", firmware_file_path, config_file_path);
    printf("Realtek: Using firmware '%s' and config '%s'\n", firmware_file_path, config_file_path);

    // activate hci callback
    hci_event_callback_registration.callback = &hci_packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);
#endif
}

static void chipset_set_baudrate_command(uint32_t baudrate, uint8_t *hci_cmd_buffer) {
    btstack_assert(baudrate == vendor_speed_to_std(rtb_cfg.vendor_baud));
    UNUSED(baudrate);

    log_info("Preparing baudrate command for %" PRIu32 " baud", baudrate);

    // hard-coded for RTL8761CTV at 3000000
    static uint8_t hci_realtek_update_baudrate_32[] = {0x17, 0xfc, 0x5 };
    memcpy (hci_cmd_buffer, hci_realtek_update_baudrate_32, sizeof(hci_realtek_update_baudrate_32));
    little_endian_store_32(hci_cmd_buffer, 3, rtb_cfg.vendor_baud);
}

void btstack_chipset_realtek_set_firmware_file_path(const char *path) {
#ifdef HAVE_POSIX_FILE_IO
    firmware_file_path = path;
#endif
}

void btstack_chipset_realtek_set_firmware_folder_path(const char *path) {
#ifdef HAVE_POSIX_FILE_IO
    firmware_folder_path = path;
#endif
}

void btstack_chipset_realtek_set_config_file_path(const char *path) {
#ifdef HAVE_POSIX_FILE_IO
    config_file_path = path;
#endif
}

void btstack_chipset_realtek_set_config_folder_path(const char *path) {
#ifdef HAVE_POSIX_FILE_IO
    config_folder_path = path;
#endif
}

void btstack_chipset_realtek_set_product_id(uint16_t id) {
    log_info("USB Product ID: 0x%04x", id);
    realtek_interface = REALTEK_INTERFACE_USB;
    product_id = id;
}

uint16_t btstack_chipset_realtek_get_num_usb_controllers(void){
    return (sizeof(fw_patch_table_usb) / sizeof(patch_info_usb)) - 1; // sentinel
}

void btstack_chipset_realtek_get_vendor_product_id(uint16_t index, uint16_t * out_vendor_id, uint16_t * out_product_id){
    btstack_assert(index < ((sizeof(fw_patch_table_usb) / sizeof(patch_info_usb)) - 1));
    *out_vendor_id = 0xbda;
    *out_product_id = fw_patch_table_usb[index].prod_id;
}

void btstack_chipset_realtek_set_local_info(uint8_t version, uint16_t revision, uint16_t subversion){
    log_info("Set Local Info for UART Controller");
    rtb_cfg.hci_version = version;
    rtb_cfg.hci_revision = revision;
    rtb_cfg.lmp_subversion = subversion;
    realtek_interface = REALTEK_INTERFACE_UART_H4;
}

uint32_t btstack_chipset_realtek_get_config_baudrate(void) {
    return vendor_speed_to_std(rtb_cfg.vendor_baud);
}

static const btstack_chipset_t btstack_chipset_realtek = {
    .name = "REALTEK",
    .init = chipset_init,
    .next_command = chipset_next_command,
    .set_baudrate_command = chipset_set_baudrate_command,
    NULL,  // chipset_set_bd_addr_command not supported or implemented yet
};

const btstack_chipset_t *btstack_chipset_realtek_instance(void) { return &btstack_chipset_realtek; }
