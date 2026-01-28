#include "commands.h"
#include "ble/ble_service.h"
#include "display_manager.h"
#include "battery.h"
#include "system_time.h"
#include <drivers/ssd1675a.h> // For debug commands if kept
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h> // For parsing helpers usage if needed, or standard lib

typedef void (*cmd_handler_t)(char *args);

struct shell_cmd {
    const char *name;
    cmd_handler_t handler;
    const char *help;
};

extern const struct shell_cmd commands[]; 

void cmd_help(char *args) {
    ble_printf("cmds:\r\n");
    for (int i=0; commands[i].name != NULL; i++) {
        ble_printf("  %s\r\n", commands[i].name);
    }
}

void cmd_cls(char *args) {
    display_manager_enable_screensaver(false);
    display_manager_clear();
    ble_printf("cleared\r\n");
}

void cmd_clean(char *args) {
    display_manager_enable_screensaver(false);
    ble_printf("cleaning...\r\n");
    display_manager_clean();
    ble_printf("done\r\n");
}

void cmd_saver(char *args) {
    display_manager_enable_screensaver(true);
    ble_printf("saver enabled\r\n");
}

void cmd_update(char *args) {
    ble_printf("updating...\r\n");
    // If screensaver is active, force update will wake thread.
    // If not, it will just update display.
    display_manager_force_update();
    ble_printf("done\r\n");
}

void cmd_text(char *args) {
    if (!args || !*args) return;
    display_manager_enable_screensaver(false);
    display_manager_show_text(args);
    ble_printf("drawn\r\n");
}

void cmd_rot(char *args) {
    if (!args) return;
    int r = atoi(args);
    display_manager_set_rotation(r);
    ble_printf("rotation: %d\r\n", r);
}

void cmd_batt(char *args) {
    int mv = battery_read_mv();
    ble_printf("bat: %d mv\r\n", mv);
}

void cmd_time(char *args) {
     if (!args || strlen(args) < 10) {
         ble_printf("usage: TIME HH:MM:SS DD.MM.YYYY\r\n");
         return;
     }
     int h, m, s, D, M, Y;
     int count = sscanf(args, "%d:%d:%d %d.%d.%d", &h, &m, &s, &D, &M, &Y);
     if (count == 6) {
         set_system_time(h, m, s, D, M, Y);
         display_manager_force_update();
         ble_printf("Time Set\r\n");
     } else {
         ble_printf("parse error\r\n");
     }
}

void cmd_debug_vcom(char *args) {
    if (!args) return;
    uint32_t val = strtoul(args, NULL, 16);
    ssd1675a_set_vcom_register((uint8_t)val);
    ble_printf("VCOM=0x%02X\r\n", (uint8_t)val);
}

void cmd_debug_lut(char *args) {
    if (!args) return;
    char *colon = strchr(args, ':');
    if (colon) {
        *colon = '\0';
        int idx = atoi(args);
        uint32_t val = strtoul(colon+1, NULL, 16);
        ssd1675a_set_lut_byte(idx, (uint8_t)val);
        ble_printf("LUT[%d]=0x%02X\r\n", idx, (uint8_t)val);
    }
}

const struct shell_cmd commands[] = {
    {"HELP", cmd_help, "List commands"},
    {"SAVER", cmd_saver, "Show Status/Saver"},
    {"CLEAR", cmd_cls, "Clear buffer"},
    {"CLEAN", cmd_clean, "Run clean cycle"},
    {"UPDATE", cmd_update, "Refresh display"},
    {"TEXT:", cmd_text, "Draw text (arg: msg)"}, 
    {"ROT:", cmd_rot, "Set Rotation 0-3"},
    {"BATT", cmd_batt, "Get Battery mV"},
    {"TIME", cmd_time, "Set Time (HH:MM:SS DD.MM.YYYY)"},
    {"DEBUG:VCOM=", cmd_debug_vcom, "Set VCOM (hex)"},
    {"DEBUG:LUT=", cmd_debug_lut, "Set LUT idx:hex"},
    {NULL, NULL, NULL}
};

void commands_init(void) {
    // Nothing to init really
}

void commands_process(const void *data, uint16_t len) {
    char input[256]; 
    uint16_t in_len = len < (sizeof(input) - 1) ? len : (sizeof(input) - 1);
    memcpy(input, data, in_len);
    input[in_len] = '\0';
    
    // Trim CRLF
    char *end = input + in_len - 1;
    while(end >= input && (*end == '\n' || *end == '\r')) *end-- = '\0';

    if (strlen(input) == 0) return;

    // Find Command
    bool found = false;
    for (int i=0; commands[i].name != NULL; i++) {
        const char *cmd = commands[i].name;
        int cmd_len = strlen(cmd);
        
        if (strncmp(input, cmd, cmd_len) == 0) {
            char *args = input + cmd_len;
            
            // Boundary matching
            if (cmd[cmd_len-1] != ':' && *args != '\0' && *args != ' ') {
                continue; 
            }
            
            while (*args == ' ') args++;
            
            commands[i].handler(args);
            found = true;
            break;
        }
    }
    
    if (!found) {
        ble_printf("unknown cmd\r\n");
    }
}
