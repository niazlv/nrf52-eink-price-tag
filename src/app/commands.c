#include "commands.h"
#include "ble/ble_service.h"
#include "display_manager.h"
#include "battery.h"
#include "system_time.h"
#include "lib/graphics.h"
#include <zephyr/bluetooth/bluetooth.h>
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
    ble_printf("done\r\n");
}

void cmd_fast(char *args) {
    ble_printf("fast update...\r\n");
    display_manager_update_partial();
    ble_printf("done\r\n");
}



void cmd_test(char *args) {
    ble_printf("Starting Partial Stress Test (Infinite)... Reset to stop.\r\n");
    display_manager_enable_screensaver(false);
    
    int32_t count = 0;
    char buf[64];
    
    // Get MAC Address
    bt_addr_le_t addr = {0};
    size_t count_id = 1;
    char addr_str[BT_ADDR_LE_STR_LEN] = "Unknown";
    
    bt_id_get(&addr, &count_id);
    bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));

    bt_id_get(&addr, &count_id);
    bt_addr_le_to_str(&addr, addr_str, sizeof(addr_str));

    // Initial Full Clean to remove artifacts
    graphics_clear(GFX_WHITE);
    graphics_draw_string(10, 10, "INITIAL CLEAN...");
    graphics_draw_string(10, 30, "Please Wait");
    
    // Use Manager's Force Update which properly calls init/display_buffer/update/sleep
    display_manager_force_update();

    int64_t last_time = k_uptime_get();

    while (1) {
        int64_t start = k_uptime_get();
        int32_t delta = (int32_t)(start - last_time);
        last_time = start;

        graphics_clear(GFX_WHITE);
        graphics_draw_string(10, 10, "PARTIAL TEST");
        
        snprintf(buf, sizeof(buf), "Frame: %d", count++);
        graphics_draw_string(10, 35, buf);
        
        snprintf(buf, sizeof(buf), "Up: %lld ms", start);
        graphics_draw_string(10, 55, buf);

        // Display MAC
        graphics_draw_string(10, 75, "MAC:");
        graphics_draw_string(10, 90, addr_str);
        
        // Delta
        snprintf(buf, sizeof(buf), "Delta: %d ms", delta);
        graphics_draw_string(10, 110, buf);

        // Use the partial update path
        display_manager_update_partial();
        
        // Small delay to allow log/uart processing if needed, though loop is blocking
        k_msleep(1); 
    }
}

void cmd_mode(char *args) {
    if (!args) return;
    int m = atoi(args);
    display_manager_set_partial_mode(m);
    ble_printf("Mode Set: %d (0=Turbo, 1=Bal, 2=Stab)\r\n", m);
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

void cmd_dsaver(char *args) {
    if (!args || !*args) {
        ble_printf("Usage: DSAVER 1 (On) or 0 (Off)\r\n");
        return;
    }
    int mode = atoi(args);
    display_manager_enable_screensaver(true); // Ensure saver is On
    display_manager_set_screensaver_mode(mode ? SCREENSAVER_MODE_DYNAMIC : SCREENSAVER_MODE_STATIC);
    ble_printf("Dynamic Saver: %s\r\n", mode ? "ON" : "OFF");
}

void cmd_anim(char *args) {
    ble_printf("Starting Animation (Reset to stop)...\r\n");
    display_manager_enable_screensaver(false);
    // Uses current partial mode (Default: Balanced)
    display_manager_set_keep_on(true); // Keep VCC On

    int x = 10, y = 10;
    int vx = 4, vy = 4;
    int size = 20;
    int frame = 0;

    // Initial Clear
    graphics_clear(GFX_WHITE);
    display_manager_force_update();

    int width = graphics_get_width();
    int height = graphics_get_height();

    while (1) {
        graphics_clear(GFX_WHITE);

        // Update Physics
        x += vx;
        y += vy;

        // Bounce
        if (x <= 0 || x + size >= width) vx = -vx;
        if (y <= 0 || y + size >= height) vy = -vy;

        // Clamp
        if (x < 0) x = 0;
        if (x + size > width) x = width - size;
        if (y < 0) y = 0;
        if (y + size > height) y = height - size;

        // Draw Box
        for (int i = 0; i < size; i++) {
            for (int j = 0; j < size; j++) {
                graphics_draw_pixel(x + i, y + j, GFX_BLACK);
            }
        }
        
        char buf[32];
        snprintf(buf, 32, "TURBO FRAME %d", frame++);
        // Draw at bottom
        graphics_draw_string(10, height - 16, buf);

        display_manager_update_partial();
        k_msleep(1);
        
        if (frame % 100 == 0) ble_printf("Anim frame %d\r\n", frame);
    }
}

const struct shell_cmd commands[] = {
    {"HELP", cmd_help, "List commands"},
    {"SAVER", cmd_saver, "Show Status/Saver"},
    {"CLEAR", cmd_cls, "Clear buffer"},
    {"CLEAN", cmd_clean, "Run clean cycle"},
    {"CLEAN", cmd_clean, "Run clean cycle"},
    {"UPDATE", cmd_update, "Refresh display"},
    {"FAST", cmd_fast, "Fast/Partial update"},
    {"TEST", cmd_test, "Infinite Stress Test"},
    {"MODE", cmd_mode, "Set Mode 0-2"},
    {"TEXT:", cmd_text, "Draw text (arg: msg)"}, 
    {"ROT:", cmd_rot, "Set Rotation 0-3"},
    {"BATT", cmd_batt, "Get Battery mV"},
    {"TIME", cmd_time, "Set Time (HH:MM:SS DD.MM.YYYY)"},
    {"DEBUG:VCOM=", cmd_debug_vcom, "Set VCOM (hex)"},
    {"DEBUG:LUT=", cmd_debug_lut, "Set LUT idx:hex"},
    {"DSAVER", cmd_dsaver, "Toggle Dynamic Saver (1=On)"},
    {"ANIM", cmd_anim, "Run Animation"},
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
