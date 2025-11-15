#ifndef MQTT_BROKER_CONNECTOR_H
#define MQTT_BROKER_CONNECTOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void MQTT_InitBrokerIP(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
void MQTT_Start(void);
void MQTT_Process(void);
uint8_t MQTT_IsConnected(void);

#ifdef __cplusplus
}
#endif

#endif // MQTT_BROKER_CONNECTOR_H
