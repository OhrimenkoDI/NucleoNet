#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Глобальные переменные состояния.
 * Определить их нужно в КАКОМ-ТО одном .c (например, в main.c),
 * а здесь только объявление.
 */
extern float http_t1;
extern float http_t2;
extern float http_t3;

extern uint8_t http_on1;
extern uint8_t http_on2;
extern uint8_t http_on3;

/**
 * Инициализация HTTP-сервера.
 * Нужно вызвать один раз после MX_LWIP_Init() и поднятия линка.
 */
void HTTP_Server_Init(void);

/**
 * Пользовательский хук: вызывается при изменении выхода через веб-страницу.
 * index: 1,2,3
 * new_state: 0 или 1
 *
 * Можно переопределить в своём модуле; слабая реализация есть в http_server.c.
 */
void HTTP_OnOutputChanged(uint8_t index, uint8_t new_state);

#ifdef __cplusplus
}
#endif

#endif // HTTP_SERVER_H
