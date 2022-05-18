//
// Created by Zikai Liu on 4/27/22.
//

#ifndef AOS_RPC_HANDLERS_H
#define AOS_RPC_HANDLERS_H

#include "aos/rpc_handler_builder.h"

enum internal_rpc_msg_type {
    INTERNAL_RPC_BIND_CORE_URPC = RPC_MSG_COUNT + 1,
    INTERNAL_RPC_REMOTE_RAM_REQUEST,
    INTERNAL_RPC_GET_LOCAL_PIDS,
    INTERNAL_RPC_MSG_COUNT
};

struct internal_rpc_bind_core_urpc_msg {
    coreid_t core;
    struct frame_identity frame;
    bool listener_first;
};

extern rpc_handler_t const rpc_handlers[INTERNAL_RPC_MSG_COUNT];

#endif  // AOS_RPC_HANDLERS_H
