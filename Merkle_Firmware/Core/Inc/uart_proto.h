#ifndef UART_PROTO_H
#define UART_PROTO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define MSG_OTA_START    0x01
#define MSG_BLOCK        0x02
#define MSG_OTA_END      0x03
#define MSG_ACK          0x10
#define MSG_NACK         0x11
#define MSG_OTA_COMPLETE 0x12
#define MSG_BLOCK_REQ    0x13
#define MSG_READY        0x14   /* MCU â†’ Server: "I am alive, send OTA_START" */
#define MSG_READ_REQ     0x20
#define MSG_READ_RESP    0x21

void proto_send(uint8_t type, const uint8_t *payload, uint16_t len);
void proto_ack(void);
void proto_nack(void);
bool proto_recv(uint8_t *type_out, uint8_t *buf, uint16_t buf_size, uint16_t *len_out, uint32_t timeout_ms);

#endif
