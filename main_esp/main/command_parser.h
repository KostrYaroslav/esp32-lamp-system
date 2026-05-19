#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CMD_ON,
    CMD_OFF,
    CMD_ALL_ON,
    CMD_ALL_OFF,
    CMD_STATUS,
    CMD_MASK_SET,
    CMD_MASK_CLR,
    CMD_HELP,
    CMD_UNKNOWN
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint32_t mask;
    int lamp_num;
} parsed_cmd_t;

parsed_cmd_t parse_command(const char *line);