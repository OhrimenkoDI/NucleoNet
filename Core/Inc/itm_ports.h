#ifndef ITM_PORTS_H
#define ITM_PORTS_H

#include <stdint.h>

void ITM_Print_Port(uint8_t port, const char *s);
void ITM_Print_Port1251(uint8_t port, const char *utf8);

#endif /* ITM_PORTS_H */
