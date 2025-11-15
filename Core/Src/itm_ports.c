#include "main.h"
#include "itm_ports.h"

static inline void ITM_SendChar_Port(uint8_t port, char ch)
{
    if (port > 31)
        return;

    // ITM включён?
    if ((ITM->TCR & ITM_TCR_ITMENA_Msk) == 0)
        return;

    // Порт разрешён?
    if ((ITM->TER & (1UL << port)) == 0)
        return;

    // Ждём готовность
    while (ITM->PORT[port].u32 == 0UL) {
        __NOP();
    }

    ITM->PORT[port].u8 = (uint8_t)ch;
}

void ITM_Print_Port(uint8_t port, const char *s)
{
    if (!s)
        return;

    while (*s) {
        ITM_SendChar_Port(port, *s++);
    }
}

void ITM_Print_Port1251(uint8_t port, const char *utf8)
{
    while (*utf8) {
        uint8_t c = (uint8_t)*utf8++;
        if ((c & 0xE0) == 0xD0 || (c & 0xE0) == 0xD1) {
            uint8_t next = (uint8_t)*utf8++;
            uint8_t ch;
            if (c == 0xD0)
                ch = next - 0x90 + 0xC0;
            else
                ch = next - 0x80 + 0xF0;
            ITM_SendChar_Port(port, ch);
        } else {
            ITM_SendChar_Port(port, c);
        }
    }
}

