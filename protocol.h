#ifndef DOOR_PROTOCOL_H
#define DOOR_PROTOCOL_H

#include <stdint.h>

/* ── WiFi credentials (both devices join the same network) ── */
#define WIFI_SSID     "YOUR_WIFI_SSID"
#define WIFI_PASS     "YOUR_WIFI_PASSWORD"

/* ── UDP port used for communication ── */
#define COMM_PORT     4210

/* ── Message types ── */
#define MSG_REQUEST   0x01   /* Door -> Controller: someone wants in */
#define MSG_APPROVE   0x02   /* Controller -> Door: approved */
#define MSG_DENY      0x03   /* Controller -> Door: denied */
#define MSG_PING      0x04   /* Either -> Either: connectivity check */
#define MSG_PONG      0x05   /* Reply to ping */

/* ── Packet (fits in one UDP datagram, no fragmentation) ── */
typedef struct __attribute__((packed)) {
    uint8_t  msg_type;
    char     person[16];     /* "Mom", "Dad", "Mason" */
    uint8_t  urgency;        /* 1-3 */
    char     reason[16];     /* "Dinner", "Help", "Entry", "Other" */
} door_packet_t;

#endif /* DOOR_PROTOCOL_H */
