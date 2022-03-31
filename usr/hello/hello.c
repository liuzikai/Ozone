/**
 * \file
 * \brief Hello world application
 */

/*
 * Copyright (c) 2016 ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, CAB F.78, Universitaetstr. 6, CH-8092 Zurich,
 * Attn: Systems Group.
 */


#include <stdio.h>

#include <aos/aos.h>
#include <aos/aos_rpc.h>
#include <unistd.h>

#define SHELL_BUF_SIZE 256

const char *large_str = "Lorem ipsum dolor sit amet, consectetur adipiscing elit, "
                        "sed do eiusmod tempor incididunt ut labore et dolore magna "
                        "aliqua. Ut enim ad minim veniam, quis nostrud exercitation "
                        "ullamco laboris nisi ut aliquip ex ea commodo consequat. "
                        "Duis aute irure dolor in reprehenderit in voluptate velit "
                        "esse cillum dolore eu fugiat nulla pariatur. Excepteur sint "
                        "occaecat cupidatat non proident, sunt in culpa qui officia "
                        "deserunt mollit anim id est laborum.";

static void print_err_if_any(errval_t err) {
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "");
    }
}

int main(int argc, char *argv[])
{
    errval_t err;
    printf("Hello, world! from userspace and through RPC, presented by AOS team 1\n");
    for (int i = 0; i < argc; i++) {
        printf("arg[%d]: %s\n", i, argv[i]);
    }

    if (argc < 2 || strcmp(argv[1], "AOS") != 0) {
        printf("Goodbye world!");
        return EXIT_SUCCESS;
    }

    printf("Entering shell since argv[1] == \"AOS\"\n");

    char buf[SHELL_BUF_SIZE];
    uword_t offset;
    while (1) {  // command loop
        putchar('$');
        putchar(' ');
        fflush(stdout);

        offset = 0;
        while (1) {  // character loop
            char c = getchar();
            if (c == '\n' || c == '\r') {
                putchar('\n');
                buf[offset] = '\0';
                if (offset == 0) {
                    // Do nothing, fall back to the next command
                } else if (strcmp(buf, "help") == 0) {
                    printf("Available commands:\n  hello\n  exit\n  send_num\n  "
                           "send_str\n  send_large_str\n  get_ram\n  "
                           "Others are interpreted as spawn commands\n");

                } else if (strcmp(buf, "exit") == 0) {
                    printf("Goodbye, world!\n");
                    return EXIT_SUCCESS;

                } else if (strcmp(buf, "send_num") == 0) {
                    printf("Trying to send number 42...\n");
                    err = aos_rpc_send_number(aos_rpc_get_init_channel(), 42);
                    print_err_if_any(err);
                    printf("Successfully send number 42\n");

                } else if (strcmp(buf, "send_str") == 0) {
                    char str[15] = "Hello RPC world";
                    printf("Trying to send a small string...\n");
                    err = aos_rpc_send_string(aos_rpc_get_init_channel(), str);
                    print_err_if_any(err);
                    printf("Successfully send string\n");

                } else if (strcmp(buf, "send_large_str") == 0) {
                    printf("Trying to send a large string...\n");
                    err = aos_rpc_send_string(aos_rpc_get_init_channel(), large_str);
                    print_err_if_any(err);
                    printf("Successfully send large string\n");

                } else if (strcmp(buf, "get_ram") == 0) {
                    size_t size = 16384;

                    printf("Trying to get a frame of size %lu...\n", size);
                    struct capref ram;
                    err = ram_alloc(&ram, size);
                    print_err_if_any(err);
                    printf("Successfully get the frame\n");

                    struct capref frame;
                    err = slot_alloc(&frame);
                    print_err_if_any(err);

                    void *addr;
                    err = cap_retype(frame, ram, 0, ObjType_Frame, size, 1);
                    print_err_if_any(err);
                    err = paging_map_frame_attr(get_current_paging_state(), &addr, size,
                                                frame, VREGION_FLAGS_READ_WRITE);
                    print_err_if_any(err);
                    printf("Mapped requested frame at %p\n", addr);

                    char *data = addr;
                    for (int i = 0; i < size; i++) {
                        if (data[i] != 0) {
                            printf("READ ERROR\n");
                        }
                        while (data[i] != 0)
                            /* hanging */;
                        data[i] = (i / 128 + i / 16) % 256;
                    }

                    for (int i = 0; i < size; i++) {
                        if (data[i] != (i / 128 + i / 16) % 256) {
                            printf("WRITE ERROR %d instead of %d\n", data[i],
                                   (i / 128 + i / 16) % 256);
                        }
                        while (data[i] != (i / 128 + i / 16) % 256)
                            /* hanging */;
                    }

                    printf("The frame is write and readable...\n");

                } else {
                    domainid_t pid = 0;
//                    printf("Spawn %s...\n", buf);
                    err = aos_rpc_process_spawn(aos_rpc_get_process_channel(), buf, 0, &pid);
//                    printf("New pid is %u\n", pid);
                    print_err_if_any(err);
                }
                break;  // prompt for the next command

            } else if (c == 127) {
                if (offset > 0) {
                    printf("\b \b");  // destructive backspace
                    offset--;
                }
            } else {
                putchar(c);  // echo
                fflush(stdout);
                buf[offset] = c;
                offset++;
                if (offset == SHELL_BUF_SIZE) {
                    printf("\nInput exceeds %d characters, resetting\n", SHELL_BUF_SIZE);
                    break;  // prompt for the next command
                }
            }
        }
    }

    return EXIT_SUCCESS;
}
