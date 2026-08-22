#ifndef APP_COMMANDS_H
#define APP_COMMANDS_H

#include <stdint.h>

/**
 * @brief Where a command arrived from / where its replies must go.
 *
 * The reply seam for the broadcast layer: handlers reply via ble_printf(), and
 * the mesh transport (Phase 1) will route those replies based on the active
 * origin (back into the flood toward the originator instead of the NUS link).
 * Phase 0 only ever uses CMD_ORIGIN_NUS.
 */
enum cmd_origin {
    CMD_ORIGIN_NUS  = 0,   /* connected NUS link (reply over current_conn) */
    CMD_ORIGIN_MESH = 1,   /* received via mesh flood (reply back into mesh) */
};

/** @brief Initialize commands module (call once at startup). */
void commands_init(void);

/**
 * @brief Run a command by opcode against the shared handler registry.
 *
 * This is the single dispatch point: both the text protocol and the binary
 * opcode frame converge here, so each command is implemented exactly once.
 *
 * @param opcode  One of the OP_* values in cmd_opcodes.h.
 * @param args    NUL-terminated argument string (same form the text handler
 *                parses). Pass "" for argument-less commands.
 * @param origin  Transport the command came from (selects the reply route).
 */
void cmd_dispatch_opcode(uint8_t opcode, char *args, enum cmd_origin origin);

/**
 * @brief Process received BLE data (text commands or binary vstream frames).
 * @param data  Raw bytes received over NUS RX.
 * @param len   Number of bytes.
 */
void commands_process(const void *data, uint16_t len);

/**
 * @brief Notify commands module that the BLE connection was lost.
 *
 * Cancels any in-progress vstream session and returns the display to the
 * screensaver so the device stays responsive after an unexpected disconnect.
 */
void commands_on_disconnect(void);

#endif // APP_COMMANDS_H
