#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "command_parser.h"

#define RELAY_COUNT 32

parsed_cmd_t parse_command(const char *line) {
    parsed_cmd_t result = { .type = CMD_UNKNOWN, .mask = 0, .lamp_num = 0 };
    uint32_t mask = 0;
    int lamp = 0;
    
    // Локальный буфер для безопасного удаления управляющих символов
    char clean_line[128];
    strncpy(clean_line, line, sizeof(clean_line) - 1);
    clean_line[sizeof(clean_line) - 1] = '\0';
    
    // Удаляем \r и \n из конца строки
    clean_line[strcspn(clean_line, "\r\n")] = '\0';
    
    if (sscanf(clean_line, "MASK_SET %" SCNx32, &mask) == 1) {
        result.type = CMD_MASK_SET;
        result.mask = mask;
    }
    else if (sscanf(clean_line, "MASK_CLR %" SCNx32, &mask) == 1) {
        result.type = CMD_MASK_CLR;
        result.mask = mask;
    }
    else if (sscanf(clean_line, "ON %d", &lamp) == 1 && lamp >= 1 && lamp <= RELAY_COUNT) {
        result.type = CMD_ON;
        result.lamp_num = lamp;
    }
    else if (sscanf(clean_line, "OFF %d", &lamp) == 1 && lamp >= 1 && lamp <= RELAY_COUNT) {
        result.type = CMD_OFF;
        result.lamp_num = lamp;
    }
    else if (strcmp(clean_line, "ALL_ON") == 0)   result.type = CMD_ALL_ON;
    else if (strcmp(clean_line, "ALL_OFF") == 0)  result.type = CMD_ALL_OFF;
    else if (strcmp(clean_line, "STATUS") == 0)   result.type = CMD_STATUS;
    else if (strcmp(clean_line, "HELP") == 0)     result.type = CMD_HELP;
    
    return result;
}