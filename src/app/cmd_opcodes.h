#ifndef APP_CMD_OPCODES_H
#define APP_CMD_OPCODES_H

/*
 * Single source of truth for command opcodes and the transport-independent
 * opcode-frame wire format.  Both the text protocol and the (future) mesh
 * broadcast layer dispatch through the SAME handler registry keyed by these
 * opcodes, so a command is implemented exactly once (text = alias for an
 * opcode).  Numbers are STABLE — never renumber an existing opcode; hosts and
 * fielded firmware agree on these values.  Append new commands at the end of
 * the relevant range.
 *
 * ── Opcode frame (inner PDU, identical on NUS and inside a mesh packet) ──
 *
 *   [opcode:1][payload:len]
 *
 * On the connected NUS byte stream the frame is delimited so it cannot be
 * confused with a text line or a vstream frame:
 *
 *   [OPC_MAGIC=0xA5][opcode:1][len:1][payload:len]
 *
 * 0xA5 is non-ASCII, so it never begins a text command; it is only treated as
 * a frame start at a line boundary (nothing buffered).  The payload for Phase 0
 * carries the same text args the handler already parses (e.g. opcode OP_MODE +
 * payload "3" replaces the 7-byte "MODE:3\n" with 4 bytes).  Per-command binary
 * arg packing is a later refinement for the broadcast-relevant subset.
 */

#define OPC_MAGIC   0xA5u   /* NUS opcode-frame start sentinel */

#define OP_INVALID  0x00u

/* ── System / identity (0x01–0x0F) ──────────────────────────────────────── */
#define OP_HELP      0x01u
#define OP_REBOOT    0x02u
#define OP_SYSINFO   0x03u
#define OP_STATS     0x04u
#define OP_DFU       0x05u
#define OP_AUTH      0x06u
#define OP_SETKEY    0x07u
#define OP_SEC       0x08u
#define OP_NAME      0x09u
#define OP_BATT      0x0Au
#define OP_TIME      0x0Bu   /* TIME and TIME= share this opcode */
#define OP_HOST      0x0Cu
#define OP_STAT      0x0Du
#define OP_BCAST     0x0Eu   /* originate a mesh broadcast (local-only command) */
#define OP_GROUP     0x0Fu   /* get/set this node's mesh group id */

/* ── Display (0x10–0x3F) ────────────────────────────────────────────────── */
#define OP_SAVER     0x10u
#define OP_SS        0x11u
#define OP_DSAVER    0x12u
#define OP_CLEAR     0x13u
#define OP_CLEAN     0x14u
#define OP_NUKE      0x15u
#define OP_UPDATE    0x16u   /* UPDATE and APPLY share this opcode */
#define OP_FAST      0x17u
#define OP_FAPPLY    0x18u
#define OP_MODE      0x19u
#define OP_PALTEST   0x1Au
#define OP_TONETEST  0x1Bu
#define OP_TEXT      0x1Cu
#define OP_ROT       0x1Du
#define OP_ANIM      0x1Eu
#define OP_TEST      0x1Fu
#define OP_VSTREAM   0x20u

/* ── LUT editor (0x40–0x5F) ─────────────────────────────────────────────── */
#define OP_LUTW      0x40u
#define OP_LW        0x41u
#define OP_LBYTE     0x42u
#define OP_LUTUSE    0x43u
#define OP_LUTSET    0x44u
#define OP_VLUT      0x45u
#define OP_LGET      0x46u
#define OP_LTEST     0x47u

/* ── Frame buffers (0x60–0x6F) ──────────────────────────────────────────── */
#define OP_FW        0x60u
#define OP_RW        0x61u

/* ── Debug (0x70–0x7F) ──────────────────────────────────────────────────── */
#define OP_VCOM      0x70u   /* VCOM= and DEBUG:VCOM= share this opcode */
#define OP_DEBUG_LUT 0x71u
#define OP_PROBE     0x72u   /* identify the panel controller (read-backs + RAM probe) */
#define OP_RULER     0x73u   /* geometry test screen (border, ticks, diagonal) */
#define OP_OTPLUT    0x74u   /* full refresh waveform: OTP (1) or working table (0) */
#define OP_SCAN      0x75u   /* scan timing: 0x3A dummy lines, 0x3B gate width */
#define OP_SPITEST   0x76u   /* display bus timing diagnostic */

/* ── Radio / power (0x80–0x8F) ──────────────────────────────────────────── */
#define OP_MESHRX    0x80u   /* get/set mesh receive (observer scan) on/off */
#define OP_PWR       0x81u   /* get/set the sleep profile: redraw schedule, adv interval */

/* 0x80–0xFF reserved for mesh-control opcodes (DISCOVER/PRESENCE/ACK, Phase 2). */

#endif /* APP_CMD_OPCODES_H */
