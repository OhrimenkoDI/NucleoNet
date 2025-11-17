#include "znp_parser.h"
#include <string.h>
#include <stdio.h>

#ifndef ZNP_RX_BUF_SZ
#define ZNP_RX_BUF_SZ 512
#endif

static uint8_t  rx_accum[ZNP_RX_BUF_SZ];
static uint32_t rx_accum_len = 0;

static uint8_t znp_fcs(const uint8_t *p, uint8_t n)
{
    uint8_t x = 0;
    for (uint8_t i = 0; i < n; i++) {
        x ^= p[i];
    }
    return x;
}

void ZNP_ResetParser(void)
{
    rx_accum_len = 0;
}

/**
 * Формат кадра:
 *  FE | LEN | CMD0 | CMD1 | DATA[LEN] | FCS
 *  всего = LEN + 5
 */
void ZNP_ParseBytes(const uint8_t *buf, uint32_t len)
{
    if (!buf || !len)
        return;

    // Переполнение — сбрасываем буфер, чтобы не поехать навсегда.
    if (len > ZNP_RX_BUF_SZ) {
        rx_accum_len = 0;
        return;
    }

    if (rx_accum_len + len > ZNP_RX_BUF_SZ) {
        rx_accum_len = 0;
    }

    memcpy(rx_accum + rx_accum_len, buf, len);
    rx_accum_len += len;

    uint32_t r = 0;

    while (rx_accum_len - r >= 5) {
        // Поиск SOF = 0xFE
        while (r < rx_accum_len && rx_accum[r] != 0xFE) {
            r++;
        }

        if (rx_accum_len - r < 5) {
            // недостаточно для минимального кадра
            break;
        }

        uint8_t L = rx_accum[r + 1];
        uint32_t need = (uint32_t)L + 5; // FE + LEN + CMD0 + CMD1 + DATA + FCS

        if (rx_accum_len - r < need) {
            // ждём остаток кадра
            break;
        }

        // Проверка FCS по LEN+CMD0+CMD1+DATA (3+L байт начиная с LEN)
        uint8_t calc_fcs = znp_fcs(&rx_accum[r + 1], (uint8_t)(3 + L));
        uint8_t recv_fcs = rx_accum[r + 4 + L];

        if (calc_fcs == recv_fcs) {
        	uint8_t cmd0 = rx_accum[r + 2];
        	uint8_t cmd1 = rx_accum[r + 3];
        	const uint8_t *payload = &rx_accum[r + 4];

        	// Передаём наверх уже проверенный кадр
        	ZNP_HandleFrame(cmd0, cmd1, payload, L);

        } else {
            // Ошибка FCS — можно залогировать при необходимости.
            printf("ZNP: bad FCS (calc=%02X recv=%02X)\r\n", calc_fcs, recv_fcs);
        }

        r += need;
    }

    if (r > 0) {
        // Сдвигаем остаток в начало буфера
        memmove(rx_accum, rx_accum + r, rx_accum_len - r);
        rx_accum_len -= r;
    }
}
