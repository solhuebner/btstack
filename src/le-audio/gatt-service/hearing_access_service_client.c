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

#define BTSTACK_FILE__ "hearing_access_service_client.c"

#include "btstack_config.h"

#ifdef ENABLE_TESTING_SUPPORT
#include <stdio.h>
#include <unistd.h>
#endif

#include <stdint.h>
#include <string.h>

#include "ble/gatt_service_client.h"
#include "le-audio/gatt-service/hearing_access_service_client.h"

#include "bluetooth_gatt.h"
#include "btstack_debug.h"
#include "btstack_event.h"

// VCS Client
static gatt_service_client_t has_client;
static btstack_linked_list_t has_connections;

static void has_client_packet_handler_internal(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void has_client_handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);
static void has_client_run_for_connection(void * context);

// list of uuids
static const uint16_t has_uuid16s[HEARING_ACCESS_SERVICE_NUM_CHARACTERISTICS] = {
    ORG_BLUETOOTH_CHARACTERISTIC_HEARING_AID_FEATURES,       
    ORG_BLUETOOTH_CHARACTERISTIC_HEARING_AID_PRESET_CONTROL_POINT,
    ORG_BLUETOOTH_CHARACTERISTIC_ACTIVE_PRESET_INDEX
};

typedef enum {
    HAS_CLIENT_CHARACTERISTIC_INDEX_START_GROUP = 0,
    HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_FEATURES = 0,
    HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT,
    HAS_CLIENT_CHARACTERISTIC_INDEX_ACTIVE_PRESET_INDEX,
    HAS_CLIENT_CHARACTERISTIC_INDEX_RFU
} has_client_characteristic_index_t;

#ifdef ENABLE_TESTING_SUPPORT
static const char * has_characteristic_names[] = {
    "HEARING_AID_FEATURES",
    "HEARING_AID_PRESET_CONTROL_POINT",
    "ACTIVE_PRESET_INDEX",
    "RFU"
};
#endif


static has_client_connection_t * has_client_get_connection_for_cid(uint16_t connection_cid){
    btstack_linked_list_iterator_t it;
    btstack_linked_list_iterator_init(&it,  &has_connections);
    while (btstack_linked_list_iterator_has_next(&it)){
        has_client_connection_t * connection = (has_client_connection_t *)btstack_linked_list_iterator_next(&it);
        if (gatt_service_client_get_connection_id(&connection->basic_connection) == connection_cid) {
            return connection;
        }
    }
    return NULL;
}

static void has_client_add_connection(has_client_connection_t * connection){
    btstack_linked_list_add(&has_connections, (btstack_linked_item_t*) connection);
}

static void has_client_finalize_connection(has_client_connection_t * connection){
    btstack_linked_list_remove(&has_connections, (btstack_linked_item_t*) connection);
}

static void has_client_replace_subevent_id_and_emit(btstack_packet_handler_t callback, uint8_t * packet, uint16_t size, uint8_t subevent_id){
    UNUSED(size);
    btstack_assert(callback != NULL);
    // execute callback
    packet[2] = subevent_id;
    (*callback)(HCI_EVENT_PACKET, 0, packet, size);
}

static void has_client_emit_connection_established(has_client_connection_t * connection, uint8_t status) {
    btstack_assert(connection != NULL);

    uint8_t event[8];
    int pos = 0;
    event[pos++] = HCI_EVENT_LEAUDIO_META;
    event[pos++] = sizeof(event) - 2;
    event[pos++] = LEAUDIO_SUBEVENT_HAS_CLIENT_CONNECTED;
    little_endian_store_16(event, pos, gatt_service_client_get_con_handle(&connection->basic_connection));
    pos += 2;
    little_endian_store_16(event, pos, gatt_service_client_get_connection_id(&connection->basic_connection));
    pos += 2;
    event[pos++] = status;
    (*connection->events_packet_handler)(HCI_EVENT_PACKET, 0, event, pos);
}

static void has_client_connected(has_client_connection_t * connection, uint8_t status) {
    if (status == ERROR_CODE_SUCCESS){
        connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_READY;
        has_client_emit_connection_established(connection, status);
    } else {
        connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_IDLE;
        has_client_emit_connection_established(connection, status);
        has_client_finalize_connection(connection);
    }
}

static void has_client_emit_uint8_value(has_client_connection_t * connection, uint8_t subevent, uint8_t data, uint8_t att_status){
    btstack_assert(connection != NULL);

    uint8_t event[7];
    uint16_t pos = 0;
    event[pos++] = HCI_EVENT_LEAUDIO_META;
    event[pos++] = 5;
    event[pos++] = subevent;
    little_endian_store_16(event, pos, gatt_service_client_get_connection_id(&connection->basic_connection));
    pos+= 2;
    event[pos++] = data;
    event[pos++] = att_status;
    (*connection->events_packet_handler)(HCI_EVENT_PACKET, 0, event, pos);
}

static void has_client_emit_generic_update(has_client_connection_t * connection,
                                           uint8_t is_last,
                                           uint8_t prev_index,
                                           const uint8_t * preset_record,
                                           uint8_t preset_record_len) {

    // parse preset record
    uint8_t preset_index = preset_record[0];
    uint8_t properties   = preset_record[1];
    const uint8_t * name    = &preset_record[2];
    uint8_t name_length  = preset_record_len - 2;

    // generate event
    uint8_t event[12 + HAS_PRESET_RECORD_NAME_MAX_LENGTH + 1];
    uint16_t pos = 0;
    event[pos++] = HCI_EVENT_LEAUDIO_META;
    event[pos++] = 0;
    event[pos++] = LEAUDIO_SUBEVENT_HAS_CLIENT_GENERIC_UPDATE;
    little_endian_store_16(event, pos, gatt_service_client_get_connection_id(&connection->basic_connection));
    pos+= 2;

    event[pos++] = is_last;     // is_last
    event[pos++] = prev_index;  // prev_index
    event[pos++] = preset_index; // newly_added_preset_index
    event[pos++] = properties & HEARING_AID_PRESET_PROPERTIES_MASK_WRITABLE;  // newly_added_preset_is_writable
    event[pos++] = properties & HEARING_AID_PRESET_PROPERTIES_MASK_AVAILABLE; // newly_added_preset_is_available

    name_length = btstack_max(HAS_PRESET_RECORD_NAME_MAX_LENGTH, name_length);
    event[pos++] = name_length;
    memcpy(&event[pos], name, name_length);
    pos += name_length;
    event[pos++] = 0;

    // update packet len
    event[1] = pos - 2;
    (*connection->events_packet_handler)(HCI_EVENT_PACKET, 0, event, pos);
}

static void has_client_emit_preset_record_changed(has_client_connection_t * connection, uint8_t subevent_id, const uint8_t * data, uint8_t data_size){
    btstack_assert(connection != NULL);

    uint8_t event[8];
    uint16_t pos = 0;
    event[pos++] = HCI_EVENT_LEAUDIO_META;
    event[pos++] = 3 + data_size;
    event[pos++] = subevent_id;
    little_endian_store_16(event, pos, gatt_service_client_get_connection_id(&connection->basic_connection));
    pos+= 2;
    // opcode[0], changeid[1], is_last[2], changed_preset_index[3]
    event[pos++] = data[0]; // opcode: PresetChanged == 0x03
    event[pos++] = data[2]; // is_last
    event[pos++] = data[3]; // changed_preset_index
    (*connection->events_packet_handler)(HCI_EVENT_PACKET, 0, event, pos);
}

static void has_client_emit_preset_record_deleted(has_client_connection_t * connection, const uint8_t * data, uint8_t data_size){
    has_client_emit_preset_record_changed(connection, LEAUDIO_SUBEVENT_HAS_CLIENT_PRESET_RECORD_DELETED, data, data_size);
}

static void has_client_emit_preset_record_available(has_client_connection_t * connection, const uint8_t * data, uint8_t data_size){
    has_client_emit_preset_record_changed(connection, LEAUDIO_SUBEVENT_HAS_CLIENT_PRESET_RECORD_AVAILABLE, data, data_size);
}

static void has_client_emit_preset_record_unavailable(has_client_connection_t * connectionr, const uint8_t * data, uint8_t data_size){
    has_client_emit_preset_record_changed(connectionr, LEAUDIO_SUBEVENT_HAS_CLIENT_PRESET_RECORD_UNAVAILABLE, data, data_size);
}

static void has_client_handle_characteristic_value(has_client_connection_t * connection, uint16_t characteristic_uuid16, const uint8_t * data, uint8_t data_size){
    switch (characteristic_uuid16){
        case ORG_BLUETOOTH_CHARACTERISTIC_HEARING_AID_FEATURES:
            if (data_size == 1){
                connection->hearing_aid_features = data[0];
            }
            break;
        case ORG_BLUETOOTH_CHARACTERISTIC_ACTIVE_PRESET_INDEX:
            if (data_size == 1){
                connection->active_preset_index = data[0];
            }
            break;
        default:
            return;
    }
}

static void has_client_emit_read_event(has_client_connection_t * connection, uint16_t characteristic_uuid16, uint8_t att_status){
    uint8_t  subevent_id;
    switch (characteristic_uuid16){
        case ORG_BLUETOOTH_CHARACTERISTIC_HEARING_AID_FEATURES:
            subevent_id = LEAUDIO_SUBEVENT_HAS_CLIENT_HEARING_AID_FEATURES;
            if (att_status != ATT_ERROR_SUCCESS) {
                has_client_emit_uint8_value(connection, subevent_id, 0, att_status);
                break;
            }
            has_client_emit_uint8_value(connection, subevent_id, connection->hearing_aid_features, ERROR_CODE_SUCCESS);
            break;

        case ORG_BLUETOOTH_CHARACTERISTIC_ACTIVE_PRESET_INDEX:
            subevent_id = LEAUDIO_SUBEVENT_HAS_CLIENT_ACTIVE_PRESET_INDEX;
            if (att_status != ATT_ERROR_SUCCESS) {
                has_client_emit_uint8_value(connection, subevent_id, 0, att_status);
                break;
            }
            has_client_emit_uint8_value(connection, subevent_id, connection->active_preset_index, ERROR_CODE_SUCCESS);
            break;

        default:
            break;
    }
}

static void has_client_handle_preset_changed_operation(has_client_connection_t * connection, const uint8_t * data, uint16_t data_size){
    switch ((has_changeid_t)data[1]){
        case HAS_CHANGEID_GENERIC_UPDATE:
            if (data_size >= 6) {
                // opcode[0], changeid[1], is_last[2], prev_index[3], index[4], properties[5], name[6]
                uint8_t is_last = data[2];
                uint8_t prev_index = data[3];
                const uint8_t * preset_record = &data[4];
                uint8_t preset_record_len = data_size - 4;
                has_client_emit_generic_update(connection, is_last, prev_index, preset_record, preset_record_len);
            }
            break;

        case HAS_CHANGEID_PRESET_RECORD_DELETED:
            if (data_size < 4){
                break;
            }
            has_client_emit_preset_record_deleted(connection, data, data_size);
            break;

        case HAS_CHANGEID_PRESET_RECORD_AVAILABLE:
            if (data_size < 4){
                break;
            }
            has_client_emit_preset_record_available(connection, data, data_size);
            break;

        case HAS_CHANGEID_PRESET_RECORD_UNAVAILABLE:
            if (data_size < 4){
                break;
            }
            has_client_emit_preset_record_unavailable(connection, data, data_size);
            break;

        default:
            return;
    }
}

static void has_client_emit_indicate_event(has_client_connection_t * connection, uint16_t value_handle, const uint8_t * data, uint16_t data_size){
    uint16_t characteristic_index = gatt_service_client_characteristic_index_for_value_handle(
            &connection->basic_connection, value_handle);
    uint16_t characteristic_uuid16 = gatt_service_client_characteristic_uuid16_for_index(&has_client, characteristic_index);

    if (characteristic_uuid16 != ORG_BLUETOOTH_CHARACTERISTIC_HEARING_AID_PRESET_CONTROL_POINT){
        return;
    }

    switch ((has_opcode_t)data[0]){
        case HAS_OPCODE_READ_PRESET_RESPONSE:
            if (data_size >= 5){
                // is_last, preset_record
                uint8_t is_last = data[1];
                const uint8_t * preset_record = &data[2];
                uint8_t preset_record_len = data_size - 2;
                uint8_t prev_index = 0xff;
                has_client_emit_generic_update(connection, is_last, prev_index, preset_record, preset_record_len);
            }
            break;
        case HAS_OPCODE_PRESET_CHANGED:
            has_client_handle_preset_changed_operation(connection, data, data_size);
            break;
        default:
            break;
    }
}

static void has_client_emit_notify_event(has_client_connection_t * connection, uint16_t value_handle, const uint8_t * data, uint16_t data_size){
    uint16_t characteristic_index = gatt_service_client_characteristic_index_for_value_handle(
            &connection->basic_connection, value_handle);
    uint16_t characteristic_uuid16 = gatt_service_client_characteristic_uuid16_for_index(&has_client, characteristic_index);
    has_client_handle_characteristic_value(connection, characteristic_uuid16, data, data_size);
    has_client_emit_read_event(connection, characteristic_uuid16,  ERROR_CODE_SUCCESS);
}

static uint8_t has_client_can_query_characteristic(has_client_connection_t * connection, has_client_characteristic_index_t characteristic_index){
    uint8_t status = gatt_service_client_can_query_characteristic(&connection->basic_connection,
                                                                  (uint8_t) characteristic_index);
    if (status != ERROR_CODE_SUCCESS){
        return status;
    }
    return connection->state == HEARING_ACCESS_SERVICE_CLIENT_STATE_READY ? ERROR_CODE_SUCCESS : ERROR_CODE_CONTROLLER_BUSY;
}

static uint8_t has_client_request_send_gatt_query(has_client_connection_t * connection, has_client_characteristic_index_t characteristic_index){
    connection->characteristic_index = characteristic_index;

    connection->query_registration.context = (void *)(uintptr_t)connection->basic_connection.cid;
    uint8_t status = gatt_client_request_to_send_gatt_query(&connection->query_registration, connection->basic_connection.con_handle);
    if (status != ERROR_CODE_SUCCESS){
        connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_READY;
    } 
    return status;
}

static uint8_t has_client_request_read_characteristic(uint16_t has_cid, has_client_characteristic_index_t characteristic_index){
    has_client_connection_t * connection = has_client_get_connection_for_cid(has_cid);
    if (connection == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    uint8_t status = has_client_can_query_characteristic(connection, characteristic_index);
    if (status != ERROR_CODE_SUCCESS){
        return status;
    }
   
    connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_W2_READ_CHARACTERISTIC_VALUE;
    return has_client_request_send_gatt_query(connection, characteristic_index);
}

static uint8_t has_client_request_write_characteristic(has_client_connection_t * connection, has_client_characteristic_index_t characteristic_index){
    log_info("has_client_request_write_characteristic, con handle 0x%04x", connection->basic_connection.con_handle);
    connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_W2_WRITE_CHARACTERISTIC_VALUE;
    return has_client_request_send_gatt_query(connection, characteristic_index);
}

static void has_client_packet_handler_internal(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET) return;
    has_client_connection_t * connection;
    uint16_t cid;
    uint8_t status;

    switch(hci_event_packet_get_type(packet)){
        case HCI_EVENT_GATTSERVICE_META:
            switch (hci_event_gattservice_meta_get_subevent_code(packet)){
                case GATTSERVICE_SUBEVENT_CLIENT_CONNECTED:
                    cid = gattservice_subevent_client_connected_get_cid(packet);
                    connection = has_client_get_connection_for_cid(cid);
                    btstack_assert(connection != NULL);

                    status = gattservice_subevent_client_connected_get_status(packet);
                    if (status != ERROR_CODE_SUCCESS){
                        has_client_connected(connection, status);
                        break;
                    }

#ifdef ENABLE_TESTING_SUPPORT
                    gatt_service_client_dump_characteristic_value_handles(&connection->basic_connection,
                                                                          has_characteristic_names);
#endif

                    if (connection->basic_connection.characteristics[HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_FEATURES].value_handle == 0){
                        connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_IDLE;
                        has_client_connected(connection, ERROR_CODE_UNSUPPORTED_FEATURE_OR_PARAMETER_VALUE);
                        break;
                    }

                    connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_READY;
                    has_client_connected(connection, ERROR_CODE_SUCCESS);
                    break;

                case GATTSERVICE_SUBEVENT_CLIENT_DISCONNECTED:
                    // TODO reset client
                    cid = gattservice_subevent_client_disconnected_get_cid(packet);
                    connection = has_client_get_connection_for_cid(cid);
                    btstack_assert(connection != NULL);
                    has_client_replace_subevent_id_and_emit(connection->events_packet_handler, packet, size, LEAUDIO_SUBEVENT_HAS_CLIENT_DISCONNECTED);
                    break;

                default:
                    break;
            }
            break;

        case GATT_EVENT_NOTIFICATION:
            cid = gatt_event_notification_get_connection_id(packet);
            connection = has_client_get_connection_for_cid(cid);
            btstack_assert(connection != NULL);
            has_client_emit_notify_event(connection, gatt_event_notification_get_value_handle(packet),
                                         gatt_event_notification_get_value(packet),
                                         gatt_event_notification_get_value_length(packet));
            break;
        
        case GATT_EVENT_INDICATION:
            cid = gatt_event_indication_get_connection_id(packet);
            connection = has_client_get_connection_for_cid(cid);
            btstack_assert(connection != NULL);
            has_client_emit_indicate_event(connection, gatt_event_indication_get_value_handle(packet),
                                         gatt_event_indication_get_value(packet),
                                         gatt_event_indication_get_value_length(packet));
            break;

        default:
            break;
    }
}

static void has_client_handle_gatt_client_event(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(packet_type); 
    UNUSED(channel);
    UNUSED(size);

    has_client_connection_t * connection = NULL;
    uint16_t connection_id;
    bool emit_value_event = false;

    switch(hci_event_packet_get_type(packet)){
        case GATT_EVENT_CHARACTERISTIC_VALUE_QUERY_RESULT:
            connection_id = (hci_con_handle_t) gatt_event_characteristic_value_query_result_get_connection_id(packet);
            connection = has_client_get_connection_for_cid(connection_id);
            btstack_assert(connection != NULL);
            has_client_handle_characteristic_value(connection,
                                                   has_uuid16s[connection->characteristic_index],
                                                   gatt_event_characteristic_value_query_result_get_value(packet),
                                                   gatt_event_characteristic_value_query_result_get_value_length(
                                                   packet));
            break;

        case GATT_EVENT_QUERY_COMPLETE:
            connection_id = gatt_event_query_complete_get_connection_id(packet);
            connection = has_client_get_connection_for_cid(connection_id);
            btstack_assert(connection != NULL);
            emit_value_event = connection->state == HEARING_ACCESS_SERVICE_CLIENT_STATE_W4_READ_CHARACTERISTIC_VALUE_RESULT;
            connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_READY;
            if (emit_value_event){
                has_client_emit_read_event(connection,
                                           has_uuid16s[connection->characteristic_index],
                                           gatt_event_query_complete_get_att_status(packet));
            }
            break;

        default:
            break;
    }
}

static void has_client_run_for_connection(void * context){
    uint16_t connection_id = (uint16_t)(uintptr_t)context;
    has_client_connection_t * connection = has_client_get_connection_for_cid(connection_id);
    btstack_assert(connection != NULL);
    log_info("has_client_run_for_connection, has_cid 0x%0x, con handle 0x%04x", connection_id, connection->basic_connection.con_handle);

    uint16_t characteristic_handle;

    switch (connection->state){
        case HEARING_ACCESS_SERVICE_CLIENT_STATE_W2_READ_CHARACTERISTIC_VALUE:
            connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_W4_READ_CHARACTERISTIC_VALUE_RESULT;
            characteristic_handle = gatt_service_client_characteristic_value_handle_for_index(
                    &connection->basic_connection, connection->characteristic_index);
            (void) gatt_client_read_value_of_characteristic_using_value_handle_with_context(
                &has_client_handle_gatt_client_event, connection->basic_connection.con_handle,
                characteristic_handle, has_client.service_id, connection_id);
            break;

        case HEARING_ACCESS_SERVICE_CLIENT_STATE_W2_WRITE_CHARACTERISTIC_VALUE:
            connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_W4_WRITE_CHARACTERISTIC_VALUE_RESULT;
            characteristic_handle = gatt_service_client_characteristic_value_handle_for_index(
                    &connection->basic_connection, connection->characteristic_index);

            (void) gatt_client_write_value_of_characteristic_with_context(
                    &has_client_handle_gatt_client_event,
                    connection->basic_connection.con_handle,
                    characteristic_handle,
                    connection->value_len,
                    connection->value_buffer,
                    has_client.service_id,
                    connection_id);
            
            break;

        default:
            break;
    }
}

void hearing_access_service_client_init(void){
    gatt_service_client_register_client(&has_client, &has_client_packet_handler_internal, has_uuid16s, sizeof(has_uuid16s)/sizeof(uint16_t));
}

uint8_t hearing_access_service_client_connect(hci_con_handle_t con_handle,
    btstack_packet_handler_t packet_handler,
    has_client_connection_t * connection,
    uint16_t * out_has_cid
){

    btstack_assert(packet_handler != NULL);
    btstack_assert(connection != NULL);

    *out_has_cid = 0;

    connection->events_packet_handler = packet_handler;
    connection->state = HEARING_ACCESS_SERVICE_CLIENT_STATE_W4_CONNECTION;
    connection->query_registration.callback = &has_client_run_for_connection;

    uint8_t status = gatt_service_client_connect_primary_service_with_uuid16(con_handle,
                                                                             &has_client, &connection->basic_connection,
                                                                             ORG_BLUETOOTH_SERVICE_HEARING_ACCESS, 0,
                                                                             connection->characteristics_storage, HEARING_ACCESS_SERVICE_NUM_CHARACTERISTICS);
    if (status == ERROR_CODE_SUCCESS){
        has_client_add_connection(connection);
        *out_has_cid = connection->basic_connection.cid;
    }

    return status;
}


uint8_t hearing_access_service_client_read_hearing_aid_features(uint16_t has_cid){
    return has_client_request_read_characteristic(has_cid, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_FEATURES);
}


uint8_t hearing_access_service_client_read_active_preset_index(uint16_t has_cid){
   return has_client_request_read_characteristic(has_cid, HAS_CLIENT_CHARACTERISTIC_INDEX_ACTIVE_PRESET_INDEX);
}


uint8_t hearing_access_service_client_read_presets_request(uint16_t has_cid, uint8_t start_index, uint8_t num_presets){
    has_client_connection_t * connection = has_client_get_connection_for_cid(has_cid);
    if (connection == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    
    uint8_t status = has_client_can_query_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
    if (status != ERROR_CODE_SUCCESS){
        return status;
    }

    if (connection->state != HEARING_ACCESS_SERVICE_CLIENT_STATE_READY){
        return ERROR_CODE_CONTROLLER_BUSY;
    }
    connection->value_buffer[0] = (uint8_t)HAS_OPCODE_READ_PRESETS_REQUEST;
    connection->value_buffer[1] = start_index;
    connection->value_buffer[2] = num_presets;
    connection->value_len = 3;
    return has_client_request_write_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
}


uint8_t hearing_access_service_client_write_preset_name(uint16_t has_cid, uint8_t index, const char * name){
    has_client_connection_t * connection = has_client_get_connection_for_cid(has_cid);
    if (connection == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    
    uint8_t status = has_client_can_query_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
    if (status != ERROR_CODE_SUCCESS){
        return status;
    }

    if (connection->state != HEARING_ACCESS_SERVICE_CLIENT_STATE_READY){
        return ERROR_CODE_CONTROLLER_BUSY;
    }

    if (name == NULL){
        return ERROR_CODE_PARAMETER_OUT_OF_MANDATORY_RANGE;
    }

    connection->value_buffer[0] = (uint8_t)HAS_OPCODE_WRITE_PRESET_NAME;
    connection->value_buffer[1] = index;
    uint8_t name_len = (uint8_t) btstack_min(strlen(name), HAS_PRESET_RECORD_NAME_MAX_LENGTH);
    memcpy(&connection->value_buffer[2], name, name_len);
    connection->value_len = 2 + name_len;
    return has_client_request_write_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
}

uint8_t hearing_access_service_client_set_active_preset(uint16_t has_cid, uint8_t index, bool synchronized_locally){
    has_client_connection_t * connection = has_client_get_connection_for_cid(has_cid);
    if (connection == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    
    uint8_t status = has_client_can_query_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
    if (status != ERROR_CODE_SUCCESS){
        return status;
    }

    if (connection->state != HEARING_ACCESS_SERVICE_CLIENT_STATE_READY){
        return ERROR_CODE_CONTROLLER_BUSY;
    }
    connection->value_buffer[0] = synchronized_locally ? (uint8_t)HAS_OPCODE_SET_ACTIVE_PRESET_SYNCHRONIZED_LOCALLY : (uint8_t)HAS_OPCODE_SET_ACTIVE_PRESET;
    connection->value_buffer[1] = index;
    connection->value_len = 2;
    return has_client_request_write_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
}

uint8_t hearing_access_service_client_set_next_preset(uint16_t has_cid, bool synchronized_locally) {
    has_client_connection_t * connection = has_client_get_connection_for_cid(has_cid);
    if (connection == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    
    uint8_t status = has_client_can_query_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
    if (status != ERROR_CODE_SUCCESS){
        return status;
    }

    if (connection->state != HEARING_ACCESS_SERVICE_CLIENT_STATE_READY){
        return ERROR_CODE_CONTROLLER_BUSY;
    }
    connection->value_buffer[0]  = synchronized_locally ? (uint8_t)HAS_OPCODE_SET_NEXT_PRESET_SYNCHRONIZED_LOCALLY : (uint8_t)HAS_OPCODE_SET_NEXT_PRESET;
    connection->value_len = 1;
    return has_client_request_write_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
}

uint8_t hearing_access_service_client_set_previous_preset(uint16_t has_cid, bool synchronized_locally) {
    has_client_connection_t * connection = has_client_get_connection_for_cid(has_cid);
    if (connection == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    
    uint8_t status = has_client_can_query_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
    if (status != ERROR_CODE_SUCCESS){
        return status;
    }

    if (connection->state != HEARING_ACCESS_SERVICE_CLIENT_STATE_READY){
        return ERROR_CODE_CONTROLLER_BUSY;
    }
    connection->value_buffer[0] = synchronized_locally ? (uint8_t)HAS_OPCODE_SET_PREVIOUS_PRESET_SYNCHRONIZED_LOCALLY : (uint8_t)HAS_OPCODE_SET_PREVIOUS_PRESET;
    connection->value_len = 1;
    return has_client_request_write_characteristic(connection, HAS_CLIENT_CHARACTERISTIC_INDEX_HEARING_AID_PRESET_CONTROL_POINT);
}

uint8_t hearing_access_service_client_disconnect(uint16_t has_cid){
    has_client_connection_t * connection = has_client_get_connection_for_cid(has_cid);
    if (connection == NULL){
        return ERROR_CODE_UNKNOWN_CONNECTION_IDENTIFIER;
    }
    return gatt_service_client_disconnect(&connection->basic_connection);
}

void hearing_access_service_client_deinit(void){
}

