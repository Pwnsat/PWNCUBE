/* Software-simulated SpaceCAN bus, ported from FlatSat's spacecan.cpp/.h
 * (2-DEFCON-Final/firmware/original/New-firmware/). Same frame format,
 * same reassembly logic, same three vulnerability mechanisms -- covers
 * VULN_CATALOG_34 findings #23 (no origin auth), #26 (reassembly overflow,
 * no bound check), #27 (cross-fragment injection, no can_id continuity
 * check), #28 (no checksum on the reassembled packet), #29 (arbitration/
 * flood DoS), #30 (bus-off attack via malformed frames). Findings #24/#25
 * (multiplexor ACL, anti-replay) live one layer below in spacecan_lib on
 * FlatSat and are not modeled here.
 *
 * No hardware involved: this is Linux userspace, not the MCU. On FlatSat
 * the MOTOR node's INJECT actually drives the real thruster API; here it's
 * independent simulated state (see spacecan_sim.c) -- the real thruster
 * lives behind the CCSDS CommandService (radio_test tcsend), untouched by
 * this bus.
 */
#ifndef PWNSAT_SPACECAN_SIM_H
#define PWNSAT_SPACECAN_SIM_H

#define SC_MAX_FRAGMENTS 43
#define SC_MAX_PACKET_SIZE 256
#define SC_MAX_DATA_LEN 8
#define SC_PAYLOAD_HEADER_LEN 2
#define SC_MAX_CHUNK_LEN (SC_MAX_DATA_LEN - SC_PAYLOAD_HEADER_LEN)
#define SC_REASSEMBLY_TIMEOUT_MS 500

#define SC_CAN_FUNCTION_MASK 0x780
#define SC_CAN_NODE_MASK 0x07F
#define SC_CANID_REQ 0x280
#define SC_CANID_REP 0x300

#define SC_NODE_SENSOR 0x01
#define SC_NODE_MOTOR 0x04
#define SC_NODE_BATTERY 0x07

/* Runs the interactive "SPACECAN>" sub-console: reads keys one at a time
 * via read_key(), echoes/handles backspace, dispatches full lines. Returns
 * when the user presses M/m at the start of an empty line (menu key), same
 * convention as the sensor dashboard. read_key() semantics match
 * read_key_timeout() in pwnsat_console.c: 0=timeout, -1=EOF, else the key. */
void spacecan_run(int (*read_key)(int timeout_s));

#endif
