/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2013 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include <stdlib.h>

#include "item.h"
#include "mock_dcp.h"

uint8_t dcp_last_op;
uint8_t dcp_last_status;
uint8_t dcp_last_nru;
uint16_t dcp_last_vbucket;
uint32_t dcp_last_opaque;
uint32_t dcp_last_flags;
uint32_t dcp_last_stream_opaque;
uint32_t dcp_last_locktime;
uint32_t dcp_last_packet_size;
uint64_t dcp_last_cas;
uint64_t dcp_last_start_seqno;
uint64_t dcp_last_end_seqno;
uint64_t dcp_last_vbucket_uuid;
uint64_t dcp_last_snap_start_seqno;
uint64_t dcp_last_snap_end_seqno;
uint64_t dcp_last_byseqno;
uint64_t dcp_last_revseqno;
uint8_t dcp_last_collection_len;
std::string dcp_last_meta;
std::string dcp_last_value;
std::string dcp_last_key;
vbucket_state_t dcp_last_vbucket_state;

static ENGINE_HANDLE *engine_handle = nullptr;
static ENGINE_HANDLE_V1 *engine_handle_v1 = nullptr;

extern "C" {

std::vector<std::pair<uint64_t, uint64_t> > dcp_failover_log;

ENGINE_ERROR_CODE mock_dcp_add_failover_log(vbucket_failover_t* entry,
                                            size_t nentries,
                                            const void *cookie) {
    (void) cookie;

    while (!dcp_failover_log.empty()) {
        dcp_failover_log.clear();
    }

    if(nentries > 0) {
        for (size_t i = 0; i < nentries; i--) {
            std::pair<uint64_t, uint64_t> curr;
            curr.first = entry[i].uuid;
            curr.second = entry[i].seqno;
            dcp_failover_log.push_back(curr);
        }
    }
   return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_get_failover_log(const void *cookie,
                                               uint32_t opaque,
                                               uint16_t vbucket) {
    (void) cookie;
    (void) opaque;
    (void) vbucket;
    clear_dcp_data();
    return ENGINE_ENOTSUP;
}

static ENGINE_ERROR_CODE mock_stream_req(const void *cookie,
                                         uint32_t opaque,
                                         uint16_t vbucket,
                                         uint32_t flags,
                                         uint64_t start_seqno,
                                         uint64_t end_seqno,
                                         uint64_t vbucket_uuid,
                                         uint64_t snap_start_seqno,
                                         uint64_t snap_end_seqno) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_STREAM_REQ;
    dcp_last_opaque = opaque;
    dcp_last_vbucket = vbucket;
    dcp_last_flags = flags;
    dcp_last_start_seqno = start_seqno;
    dcp_last_end_seqno = end_seqno;
    dcp_last_vbucket_uuid = vbucket_uuid;
    dcp_last_packet_size = 64;
    dcp_last_snap_start_seqno = snap_start_seqno;
    dcp_last_snap_end_seqno = snap_end_seqno;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_add_stream_rsp(const void *cookie,
                                             uint32_t opaque,
                                             uint32_t stream_opaque,
                                             uint8_t status) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_ADD_STREAM;
    dcp_last_opaque = opaque;
    dcp_last_stream_opaque = stream_opaque;
    dcp_last_status = status;
    dcp_last_packet_size = 28;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_snapshot_marker_resp(const void *cookie,
                                                   uint32_t opaque,
                                                   uint8_t status) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER;
    dcp_last_opaque = opaque;
    dcp_last_status = status;
    dcp_last_packet_size = 24;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_stream_end(const void *cookie,
                                         uint32_t opaque,
                                         uint16_t vbucket,
                                         uint32_t flags) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_STREAM_END;
    dcp_last_opaque = opaque;
    dcp_last_vbucket = vbucket;
    dcp_last_flags = flags;
    dcp_last_packet_size = 28;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_marker(const void *cookie,
                                     uint32_t opaque,
                                     uint16_t vbucket,
                                     uint64_t snap_start_seqno,
                                     uint64_t snap_end_seqno,
                                     uint32_t flags) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_SNAPSHOT_MARKER;
    dcp_last_opaque = opaque;
    dcp_last_vbucket = vbucket;
    dcp_last_packet_size = 44;
    dcp_last_snap_start_seqno = snap_start_seqno;
    dcp_last_snap_end_seqno = snap_end_seqno;
    dcp_last_flags = flags;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_mutation(const void* cookie,
                                       uint32_t opaque,
                                       item* itm,
                                       uint16_t vbucket,
                                       uint64_t by_seqno,
                                       uint64_t rev_seqno,
                                       uint32_t lock_time,
                                       const void* meta,
                                       uint16_t nmeta,
                                       uint8_t nru,
                                       uint8_t collectionLen) {
    (void) cookie;
    clear_dcp_data();
    Item* item = reinterpret_cast<Item*>(itm);
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_MUTATION;
    dcp_last_opaque = opaque;
    dcp_last_key.assign(item->getKey().c_str());
    dcp_last_vbucket = vbucket;
    dcp_last_byseqno = by_seqno;
    dcp_last_revseqno = rev_seqno;
    dcp_last_locktime = lock_time;
    dcp_last_meta.assign(static_cast<const char*>(meta), nmeta);
    dcp_last_value.assign(static_cast<const char*>(item->getData()),
                          item->getNBytes());
    dcp_last_nru = nru;

    // @todo: MB-24391: We are querying the header length with collections
    // off, which if we extended our testapp tests to do collections may not be
    // correct. For now collections testing is done via GTEST tests and isn't
    // reliant on dcp_last_packet_size so this doesn't cause any problems.
    dcp_last_packet_size =
            protocol_binary_request_dcp_mutation::getHeaderLength(false);
    dcp_last_packet_size = dcp_last_packet_size + dcp_last_key.length() +
                           item->getNBytes() + nmeta;

    dcp_last_collection_len = collectionLen;
    if (engine_handle_v1 && engine_handle) {
        engine_handle_v1->release(engine_handle, NULL, item);
    }
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_deletion(const void* cookie,
                                       uint32_t opaque,
                                       item* itm,
                                       uint16_t vbucket,
                                       uint64_t by_seqno,
                                       uint64_t rev_seqno,
                                       const void* meta,
                                       uint16_t nmeta,
                                       uint8_t collectionLen) {
    (void) cookie;
    clear_dcp_data();
    Item* item = reinterpret_cast<Item*>(itm);
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_DELETION;
    dcp_last_opaque = opaque;
    dcp_last_key.assign(item->getKey().c_str());
    dcp_last_cas = item->getCas();
    dcp_last_vbucket = vbucket;
    dcp_last_byseqno = by_seqno;
    dcp_last_revseqno = rev_seqno;
    dcp_last_meta.assign(static_cast<const char*>(meta), nmeta);

    // @todo: MB-24391 as above.
    dcp_last_packet_size = dcp_last_key.length() + item->getNBytes() + nmeta;
    dcp_last_packet_size +=
            protocol_binary_request_dcp_deletion::getHeaderLength(false);

    dcp_last_value.assign(static_cast<const char*>(item->getData()),
                          item->getNBytes());
    dcp_last_collection_len = collectionLen;

    if (engine_handle_v1 && engine_handle) {
        engine_handle_v1->release(engine_handle, nullptr, item);
    }

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_expiration(const void*,
                                         uint32_t,
                                         item* itm,
                                         uint16_t,
                                         uint64_t,
                                         uint64_t,
                                         const void*,
                                         uint16_t,
                                         uint8_t) {
    clear_dcp_data();
    if (engine_handle_v1 && engine_handle) {
        engine_handle_v1->release(engine_handle, nullptr, itm);
    }
    return ENGINE_ENOTSUP;
}

static ENGINE_ERROR_CODE mock_flush(const void* cookie,
                                    uint32_t opaque,
                                    uint16_t vbucket) {
    (void) cookie;
    (void) opaque;
    (void) vbucket;
    clear_dcp_data();
    return ENGINE_ENOTSUP;
}

static ENGINE_ERROR_CODE mock_set_vbucket_state(const void* cookie,
                                                uint32_t opaque,
                                                uint16_t vbucket,
                                                vbucket_state_t state) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_SET_VBUCKET_STATE;
    dcp_last_opaque = opaque;
    dcp_last_vbucket = vbucket;
    dcp_last_vbucket_state = state;
    dcp_last_packet_size = 25;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_noop(const void* cookie,
                                   uint32_t opaque) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_NOOP;
    dcp_last_opaque = opaque;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_buffer_acknowledgement(const void* cookie,
                                                     uint32_t opaque,
                                                     uint16_t vbucket,
                                                     uint32_t buffer_bytes) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_BUFFER_ACKNOWLEDGEMENT;
    dcp_last_opaque = opaque;
    dcp_last_vbucket = vbucket;
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_control(const void* cookie,
                                           uint32_t opaque,
                                           const void *key,
                                           uint16_t nkey,
                                           const void *value,
                                           uint32_t nvalue) {
    (void) cookie;
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_DCP_CONTROL;
    dcp_last_opaque = opaque;
    dcp_last_key.assign(static_cast<const char*>(key), nkey);
    dcp_last_value.assign(static_cast<const char*>(value), nvalue);
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_system_event(const void* cookie,
                                           uint32_t opaque,
                                           uint16_t vbucket,
                                           mcbp::systemevent::id event,
                                           uint64_t bySeqno,
                                           cb::const_byte_buffer key,
                                           cb::const_byte_buffer eventData) {
    (void)cookie;
    clear_dcp_data();
    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE mock_get_error_map(const void* cookie,
                                            uint32_t opaque,
                                            uint16_t version) {
    clear_dcp_data();
    dcp_last_op = PROTOCOL_BINARY_CMD_GET_ERROR_MAP;
    return ENGINE_SUCCESS;
}
}

void clear_dcp_data() {
    dcp_last_op = 0;
    dcp_last_status = 0;
    dcp_last_nru = 0;
    dcp_last_vbucket = 0;
    dcp_last_opaque = 0;
    dcp_last_flags = 0;
    dcp_last_stream_opaque = 0;
    dcp_last_locktime = 0;
    dcp_last_cas = 0;
    dcp_last_start_seqno = 0;
    dcp_last_end_seqno = 0;
    dcp_last_vbucket_uuid = 0;
    dcp_last_snap_start_seqno = 0;
    dcp_last_snap_end_seqno = 0;
    dcp_last_meta.clear();
    dcp_last_value.clear();
    dcp_last_key.clear();
    dcp_last_vbucket_state = (vbucket_state_t)0;
}

std::unique_ptr<dcp_message_producers> get_dcp_producers(ENGINE_HANDLE *_h,
                                                         ENGINE_HANDLE_V1 *_h1) {
    std::unique_ptr<dcp_message_producers> producers(new dcp_message_producers);

    producers->get_failover_log = mock_get_failover_log;
    producers->stream_req = mock_stream_req;
    producers->add_stream_rsp = mock_add_stream_rsp;
    producers->marker_rsp = mock_snapshot_marker_resp;
    producers->stream_end = mock_stream_end;
    producers->marker = mock_marker;
    producers->mutation = mock_mutation;
    producers->deletion = mock_deletion;
    producers->expiration = mock_expiration;
    producers->flush = mock_flush;
    producers->set_vbucket_state = mock_set_vbucket_state;
    producers->noop = mock_noop;
    producers->buffer_acknowledgement = mock_buffer_acknowledgement;
    producers->control = mock_control;
    producers->system_event = mock_system_event;
    producers->get_error_map = mock_get_error_map;

    engine_handle = _h;
    engine_handle_v1 = _h1;
    return producers;
}
