/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
#include <string.h>
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "lwip.h"
#include "usb_host.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "lwip/udp.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "mqtt_broker_connector.h"
#include "lwip/ip4_addr.h"
#include "itm_ports.h"

static struct udp_pcb *g_upcb = NULL;
static ip_addr_t g_dst_ip;
#define UDP_DST_PORT 5005

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
#include "usb_host.h"
#include "usbh_cdc.h"

#include <string.h>
#include <stdio.h>

extern ApplicationTypeDef Appli_state;  // только extern, без инициализации
extern uint8_t  dummy_rx[256];
extern volatile uint8_t  cdc_rx_ready;
extern volatile uint32_t cdc_rx_len;
extern uint8_t  cdc_tx_buf[256];
extern USBH_HandleTypeDef hUsbHostFS;
extern uint16_t plugNwk;


/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
void MX_USB_HOST_Process(void);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

// Функция для ITM (SWO)
int _write(int file, char *ptr, int len)
{
    for (int i = 0; i < len; i++) {
        ITM_SendChar((uint32_t)*ptr++);
    }
    return len;
}

// для http_server ----------------------------------
#include "http_server.h"

// (опционально) своя реализация хука:
void HTTP_OnOutputChanged(uint8_t index, uint8_t new_state)
{
    char buf[64];
    snprintf(buf, sizeof(buf),
             "EVENT: socket %u -> %u\r\n",
             (unsigned)index, (unsigned)new_state);
    ITM_Print_Port(2, buf);

    // здесь же можешь включать/выключать реальные выходы GPIO
}
// для http_server ----------------------------------




/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */


  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_LWIP_Init();
  MX_USB_HOST_Init();
  /* USER CODE BEGIN 2 */

  HAL_GPIO_WritePin(LEDB_GPIO_Port,LEDB_Pin,1);
  HAL_GPIO_WritePin(LEDR_GPIO_Port,LEDR_Pin,1);
  HAL_GPIO_WritePin(USBPow_GPIO_Port,USBPow_Pin,1);
  HAL_Delay(100);

  extern struct netif gnetif;
  uint32_t t0 = HAL_GetTick();
  while (!netif_is_link_up(&gnetif) && (HAL_GetTick() - t0 < 10000)) {
      MX_LWIP_Process();
  }


  // Цель: например, ПК с netcat:  nc -ul 5005
  IP_ADDR4(&g_dst_ip, 192,168,0,100); // замени на адрес ПК/сервера
  g_upcb = udp_new();
  if (g_upcb) {
    udp_connect(g_upcb, &g_dst_ip, UDP_DST_PORT);
  }

  // Настраиваем IP брокера (сюда IP машины с mosquitto)
  MQTT_InitBrokerIP(192, 168, 0, 100);   // пример

  // Подключаемся к MQTT брокеру
  MQTT_Start();

  // Сервер HTTP
  HTTP_Server_Init();   // <= вот это добавить

  ITM_Print_Port(0, "Main Start\r\n");



  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  uint32_t last = 0;
  uint32_t last_led = 0;
  uint8_t  started = 0;
  uint32_t last_ping = 0;

  while (1)
  {
	  MX_LWIP_Process();   // генерит ethernetif_input() + sys_check_timeouts()
	  MQTT_Process();           // MQTT-телеметрия
	  MX_USB_HOST_Process();


	  if (g_upcb && (HAL_GetTick() - last) >= 1000*60) {  // раз в секунду
	      const char *msg = "Hello from F767 over UDP\n";
	      struct pbuf *pb = pbuf_alloc(PBUF_TRANSPORT, strlen(msg), PBUF_RAM);
	      if (pb) {
	        memcpy(pb->payload, msg, pb->len);
	        udp_send(g_upcb, pb);
	        pbuf_free(pb);
	      }
	      last = HAL_GetTick();
	  }
    /* USER CODE END WHILE */
    MX_USB_HOST_Process();

    /* USER CODE BEGIN 3 */
    // мигаем без блокировок
    uint32_t now = HAL_GetTick();
    if (now - last_led > 200) {
        last_led = now;
        HAL_GPIO_TogglePin(LEDB_GPIO_Port, LEDB_Pin);
        HAL_GPIO_TogglePin(LEDR_GPIO_Port, LEDR_Pin);
        ITM_Print_Port(0, "ZNP RX frame\r\n");      // старый лог
        ITM_Print_Port(1, "Parser OK\r\n");        // отдельное окно под парсер
        ITM_Print_Port(1, "On/Off cmd sent\r\n");

    }

    if (Appli_state == APPLICATION_READY)
    {
    	if (!started) {
    	    HAL_Delay(50);

    	    if (Send_EF_raw() == 0) {
    	        printf("EF sent\r\n");
    	    }

    	    uint32_t t0 = HAL_GetTick();
    	    while ((HAL_GetTick() - t0) < 500) {
    	        MX_USB_HOST_Process();
    	        if (cdc_rx_ready) {
    	            uint32_t n = cdc_rx_len;
    	            cdc_rx_ready = 0;

    	            printf("RX RAW:");
    	            for (uint32_t i = 0; i < n; i++) {
    	                printf(" %02X", dummy_rx[i]);
    	            }
    	            printf("\r\n");

    	            ZNP_ParseBytes(dummy_rx, n);
    	            USBH_CDC_Receive(&hUsbHostFS, dummy_rx, sizeof(dummy_rx));
    	        }
    	    }

    	    ZNP_Send_SYS_PING();                 // контроль линка
    	    ZB_START_REQUEST();
    	    ZNP_AF_REGISTER();                   // <--- ВАЖНО: регаем APP_EP
    	    ZNP_Send_ZDO_NWK_ADDR_REQ_Plug();    // ищем NWK для розетки

    	    started = 1;
    	}


    	if (cdc_rx_ready) {
    	    uint32_t n = cdc_rx_len;
    	    cdc_rx_ready = 0;

    	    printf("RX RAW:");
    	    for (uint32_t i = 0; i < n; i++) {
    	        printf(" %02X", dummy_rx[i]);
    	    }
    	    printf("\r\n");

    	    ZNP_ParseBytes(dummy_rx, n);
    	    USBH_CDC_Receive(&hUsbHostFS, dummy_rx, sizeof(dummy_rx));
    	}


        static uint32_t last_toggle = 0;
        static uint8_t state = 0;

        if (plugNwk && (now - last_toggle > 3000)) { // раз в 3 секунды
            last_toggle = now;
            state = !state;
            send_onoff(state);
        }

        if (now - last_ping > 5000) {
            last_ping = now;
            ZNP_Send_SYS_PING();
        }

    }else
    if (Appli_state == APPLICATION_DISCONNECT){
    	started = 0;
    }
    /* USER CODE END 3 */
  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 216;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 9;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, LEDR_Pin|LEDB_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(USBPow_GPIO_Port, USBPow_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : LEDR_Pin LEDB_Pin */
  GPIO_InitStruct.Pin = LEDR_Pin|LEDB_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : USBPow_Pin */
  GPIO_InitStruct.Pin = USBPow_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USBPow_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
