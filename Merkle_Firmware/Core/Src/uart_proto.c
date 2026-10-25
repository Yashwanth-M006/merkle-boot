#include "uart_proto.h"
#include "stm32f4xx_hal.h"
#include <string.h>

#define SOF 0xAA

extern UART_HandleTypeDef huart2;

/* CRC-16/ARC — poly 0x8005 reflected (0xA001), init 0xFFFF.
   Identical to the Python crc16() in uart_server.py. */
static uint16_t crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

static uint16_t crc16_cont(uint16_t crc, const uint8_t *data, uint32_t len)
{
    while (len--) {
        crc ^= *data++;
        for (int i = 0; i < 8; i++)
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

/* Send: [0xAA][type(1)][len(2 LE)][payload][crc16(2 LE)] */
void proto_send(uint8_t type, const uint8_t *payload, uint16_t len)
{
    uint8_t hdr[4] = { SOF, type, len & 0xFF, (len >> 8) & 0xFF };
    uint16_t c = crc16(hdr, 4);
    if (len)
        c = crc16_cont(c, payload, len);

    HAL_UART_Transmit(&huart2, hdr, 4, 100);
    if (len)
        HAL_UART_Transmit(&huart2, (uint8_t*)payload, len, 5000);
    uint8_t crc_b[2] = { c & 0xFF, (c >> 8) & 0xFF };
    HAL_UART_Transmit(&huart2, crc_b, 2, 100);
}

void proto_ack(void)  { proto_send(MSG_ACK,  NULL, 0); }
void proto_nack(void) { proto_send(MSG_NACK, NULL, 0); }

bool proto_recv(uint8_t *type_out,
                uint8_t *buf, uint16_t buf_size,
                uint16_t *len_out, uint32_t timeout_ms)
{
    uint8_t b;
    uint32_t max_tries = timeout_ms / 10; if (max_tries == 0) max_tries = 1; for (uint32_t tries = 0; tries < max_tries; tries++) {
        if (HAL_UART_Receive(&huart2, &b, 1, 10) == HAL_OK && b == SOF)
            goto got_sof;
    }
    return false;

got_sof:;
    uint8_t hdr[3];
    if (HAL_UART_Receive(&huart2, hdr, 3, 500) != HAL_OK) return false;
    uint8_t  type = hdr[0];
    uint16_t len  = hdr[1] | ((uint16_t)hdr[2] << 8);

    if (len > buf_size) return false;
    if (len && HAL_UART_Receive(&huart2, buf, len, 10000) != HAL_OK)
        return false;

    uint8_t crc_b[2];
    if (HAL_UART_Receive(&huart2, crc_b, 2, 500) != HAL_OK) return false;
    uint16_t recv_crc = crc_b[0] | ((uint16_t)crc_b[1] << 8);

    uint8_t hdr_full[4] = { SOF, type, hdr[1], hdr[2] };
    uint16_t local = crc16(hdr_full, 4);
    if (len)
        local = crc16_cont(local, buf, len);

    if (local != recv_crc) return false;

    *type_out = type;
    *len_out  = len;
    return true;
}
