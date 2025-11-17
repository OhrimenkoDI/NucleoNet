#ifndef ZNP_PARSER_H
#define ZNP_PARSER_H

#include <stdint.h>

void ZNP_ParseBytes(const uint8_t *buf, uint32_t len);
void ZNP_ResetParser(void);

/* Колбэк обработки уже проверенного кадра */
void ZNP_HandleFrame(uint8_t cmd0, uint8_t cmd1,
                     const uint8_t *payload, uint8_t len);

#endif // ZNP_PARSER_H
