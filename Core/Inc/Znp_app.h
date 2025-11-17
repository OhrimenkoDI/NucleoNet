#ifndef ZNP_APP_H
#define ZNP_APP_H

#include <stdint.h>

void send_onoff(uint8_t on);
void ZB_START_REQUEST(void);
void ZNP_AF_REGISTER(void);
int ZNP_Send(uint8_t cmd0, uint8_t cmd1, const uint8_t *data, uint8_t len);
void ZNP_Send_SYS_RESET_REQ(uint8_t type /*0x00=Hard,0x01=Soft*/);
int ZNP_Send_SYS_PING(void);
int Send_EF_raw(void);
void ZNP_Send_ZDO_NWK_ADDR_REQ_Plug(void);

typedef struct
{
    uint16_t group_id;        // GroupID
    uint16_t cluster_id;      // ClusterID
    uint16_t src_addr;        // SrcAddr
    uint8_t  src_endpoint;    // SrcEndpoint
    uint8_t  dst_endpoint;    // DstEndpoint
    uint8_t  was_broadcast;   // WasBroadcast
    uint8_t  lqi;             // LinkQuality
    uint8_t  security_use;    // SecurityUse
    uint32_t timestamp;       // TimeStamp
    uint8_t  trans_seq;       // TransSeqNumber
    uint8_t  data_len;        // DataLen
    const uint8_t *data;      // -> Data[0]
} AfIncomingMsg;



#endif
