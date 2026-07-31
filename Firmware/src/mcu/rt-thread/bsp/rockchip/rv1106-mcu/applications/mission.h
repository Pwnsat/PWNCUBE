/*
 * CubeSat — Mission parameters (on-air and CCSDS mission parameters)
 */

#ifndef MISSION_H
#define MISSION_H

#include <stdint.h>

/* Firmware version */
#define FW_PATCH  0
#define FW_MINOR  0
#define FW_MAJOR  1

#define SPACECRAFT_ID  0x01

/* Uplink (radio0): always listening for TC */
#define UPLINK_FREQ     918000000U
#define UPLINK_SF       7
#define UPLINK_BW       250000U       /* BW250 — FlatSat_Firmware ruplink.h UPLINK_BW=250 */
#define UPLINK_CR       1       /* 4/5 (RadioLib setCodingRate(5)) */

/* Downlink (radio1): telemetry TX + TC response TX */
#define DOWNLINK_FREQ   916000000U
#define DOWNLINK_SF     7
#define DOWNLINK_BW     250000U       /* BW250 — FlatSat_Firmware rdownlink.h DOWNLINK_BW=250 */
#define DOWNLINK_CR     1       /* 4/5 */
#define DOWNLINK_POWER  20

/* Timer intervals (ms) */
#define TELEM_INTERVAL_MS    10500U
#define SYNC_INTERVAL_MS     15000U
#define IDLE_INTERVAL_MS     20000U
#define BEACON_INTERVAL_MS   15000U   /* default, configurable via TC */

/* APIDS TC */
#define SPP_APID_TC_PING            0x01
#define SPP_APID_TC_RESETC          0x02
#define SPP_APID_TC_SEND_FW         0x03
#define SPP_APID_TC_SET_THRUSTER    0x04
#define SPP_APID_TC_SET_BEACON_RATE 0x05
#define SPP_APID_TC_BROADCAST_MSG   0x06
#define SPP_APID_TC_FLASH           0x07
/* 0x08/0x09 reserved (TM_SEND_TM / TM_UNKNOWN, TM-only in practice but keep
 * the TC/TM numbering space free of collisions). */
#define SPP_APID_TC_GPS_OVERRIDE    0x0A  /* debug hook, Etapa 3 -- see
                                            * command_apid_handler(). Not a
                                            * real GPS input path (no
                                            * antenna, no exploit); ported
                                            * from FlatSat's own debug-only
                                            * GPS_OVERRIDE (2-DEFCON-Final,
                                            * firmware/gps-debug-demo/).
                                            * Real driver: Etapa 5,
                                            * gps_nmea.c/.h (NEO-6M/UART0) --
                                            * still wins over it while
                                            * active, see command_service.c. */

/* APIDS TM */
#define SPP_APID_TM_PING            0x01
#define SPP_APID_TM_RESETC          0x02
#define SPP_APID_TM_SEND_FW         0x03
#define SPP_APID_TM_SET_THRUSTER    0x04
#define SPP_APID_TM_SET_BEACON_RATE 0x05
#define SPP_APID_TM_BROADCAST_MSG   0x06
#define SPP_APID_TM_FLASH           0x07
#define SPP_APID_TM_SEND_TM         0x08
#define SPP_APID_TM_UNKNOWN         0x09
#define SPP_APID_TM_NAV             0x0B  /* GPS/NAV telemetry, Etapa 3+5 */

/* Etapa 6.2 -- mission/ground-station parity with FlatSat. Same APID
 * numbers FlatSat's real firmware uses for the same concepts (see
 * 2-DEFCON-Final/firmware/original/New-firmware/mission.h), reused as-is
 * (no collision with anything PWNCUBE already defined above) so
 * PWNSAT-C3's existing decoders (tm_decoder.py) work against PWNCUBE
 * without modification -- this is a straight port of
 * worker.cpp's STATUS/MISSION_MODE/PAYLOAD_STATUS/GS_MODE/GS_ACCESS/
 * GS_STATUS TC handlers + TM builders into command_service.c, same byte
 * layouts, same ground-station auth logic (static XOR key, same
 * fictitious Las Vegas coordinates). See command_service.c for the
 * implementation. TC and TM share the same numeric APID here, same as
 * every other command in this firmware -- distinguished by the SPP
 * header's packet_type bit, not by a different number. */
#define SPP_APID_TC_GET_STATUS         0x0C
#define SPP_APID_TM_STATUS             0x0C
#define SPP_APID_TC_SET_MISSION_MODE   0x0D
#define SPP_APID_TM_MISSION_MODE       0x0D
#define SPP_APID_TC_GET_PAYLOAD_STATUS 0x0F
#define SPP_APID_TM_PAYLOAD_STATUS     0x0F
#define SPP_APID_TC_GS_MODE            0x11
#define SPP_APID_TM_GS_MODE            0x11
#define SPP_APID_TC_GS_ACCESS          0x12
#define SPP_APID_TM_GS_ACCESS          0x12
#define SPP_APID_TC_GS_STATUS          0x13
#define SPP_APID_TM_GS_STATUS          0x13

#define MISSION_MODE_SAFE         0x00
#define MISSION_MODE_NOMINAL      0x01
#define MISSION_MODE_PAYLOAD      0x02
#define MISSION_MODE_SCIENCE      0x03
#define MISSION_MODE_CONTINGENCY  0x04

/* status_flags bitmask (STATUS/MISSION_MODE TMs) -- same bits FlatSat
 * uses (see mission.h there), kept 1:1 so tm_decoder.py's mission_flags()
 * decodes them without changes. BME_OK/ACC_OK are reported unconditionally
 * true, matching this firmware's existing "errors are never checked"
 * stance elsewhere (command_service_telemetry_worker). SECURE_LINK and
 * USB_DEBUG stay permanently 0 -- PWNCUBE has neither an AES-ECB link nor
 * a USB-vs-radio command source distinction -- honestly reporting "off"
 * rather than faking a subsystem that doesn't exist. */
#define MISSION_FLAG_BME_OK          0x01
#define MISSION_FLAG_ACC_OK          0x02
#define MISSION_FLAG_GPS_UART_OK     0x04
#define MISSION_FLAG_GPS_FIX         0x08
#define MISSION_FLAG_SECURE_LINK     0x10
#define MISSION_FLAG_PAYLOAD_ARMED   0x20
#define MISSION_FLAG_USB_DEBUG       0x40
#define MISSION_FLAG_GPS_NMEA_ACTIVE 0x80

/* gps_status_flags bitmask -- same bits FlatSat uses. These bits (and
 * mission_status_flags() above) now also reflect the real GPS driver
 * (Etapa 5) when it has UART/fix activity -- but the GS_ACCESS gate
 * itself (gs_gps_usable()) is deliberately kept keyed ONLY to the Etapa 3
 * debug override (s_gps_override_active), by design, confirmed with
 * Romel 2026-07-30 -- see gs_gps_usable()'s own comment in
 * command_service.c. */
#define GPS_STATUS_UART_OK     0x01
#define GPS_STATUS_CONNECTED   0x02
#define GPS_STATUS_NMEA_ACTIVE 0x04
#define GPS_STATUS_FIX_VALID   0x08
#define GPS_STATUS_TIME_VALID  0x10

/* gs_status_flags bitmask (GS_MODE/GS_ACCESS/GS_STATUS TMs) -- same bits
 * FlatSat uses. */
#define GS_STATUS_MODE_ENABLED      0x01
#define GS_STATUS_GPS_VALID         0x02
#define GS_STATUS_WITHIN_RANGE      0x04
#define GS_STATUS_AUTH_ACTIVE       0x08
#define GS_STATUS_HANDSHAKE_PENDING 0x10
#define GS_STATUS_GATE_OPEN         0x20

#define MAX_PAYLOAD_CHUNK 128

#endif /* MISSION_H */
