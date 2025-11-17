/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file            : usb_host.c
  * @version         : v1.0_Cube
  * @brief           : This file implements the USB Host
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
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/

#include "usb_host.h"
#include "usbh_core.h"
#include "usbh_cdc.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* USER CODE BEGIN PV */
/* Private variables ---------------------------------------------------------*/

/* USER CODE END PV */

/* USER CODE BEGIN PFP */
/* Private function prototypes -----------------------------------------------*/

/* USER CODE END PFP */

/* USB Host core handle declaration */
USBH_HandleTypeDef hUsbHostFS;
ApplicationTypeDef Appli_state = APPLICATION_IDLE;

/*
 * -- Insert your variables declaration here --
 */
/* USER CODE BEGIN 0 */

uint8_t dummy_rx[256];                 // глобальный буфер приёма
volatile uint8_t  cdc_rx_ready = 0;    // флаг готовности
volatile uint32_t cdc_rx_len   = 0;    // длина принятого пакета

// --- TX side (глобальные) ---
uint8_t          cdc_tx_buf[256];
volatile uint8_t cdc_tx_busy = 0;

// Если в твоей версии стека есть weak-колбэк — он снимет busy сразу по окончании передачи.
// Если его нет — ничего страшного, будем опрашивать состояние (см. код ниже).
void USBH_CDC_TransmitCallback(USBH_HandleTypeDef *phost)
{
  cdc_tx_busy = 0;
}

void USBH_CDC_ReceiveCallback(USBH_HandleTypeDef *phost)
{
  cdc_rx_len   = USBH_CDC_GetLastReceivedDataSize(phost);
  cdc_rx_ready = 1;
//  printf("USBH_CDC_ReceiveCallback cdc_rx_len=%d\n", cdc_rx_len);
}

/* USER CODE END 0 */

/*
 * user callback declaration
 */
static void USBH_UserProcess(USBH_HandleTypeDef *phost, uint8_t id);

/*
 * -- Insert your external function declaration here --
 */
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/**
  * Init USB host library, add supported class and start the library
  * @retval None
  */
void MX_USB_HOST_Init(void)
{
  /* USER CODE BEGIN USB_HOST_Init_PreTreatment */

  /* USER CODE END USB_HOST_Init_PreTreatment */

  /* Init host Library, add supported class and start the library. */
  if (USBH_Init(&hUsbHostFS, USBH_UserProcess, HOST_FS) != USBH_OK)
  {
    Error_Handler();
  }
  if (USBH_RegisterClass(&hUsbHostFS, USBH_CDC_CLASS) != USBH_OK)
  {
    Error_Handler();
  }
  if (USBH_Start(&hUsbHostFS) != USBH_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USB_HOST_Init_PostTreatment */

  /* USER CODE END USB_HOST_Init_PostTreatment */
}

/*
 * Background task
 */
void MX_USB_HOST_Process(void)
{
  /* USB Host Background task */
  USBH_Process(&hUsbHostFS);
}
/*
 * user callback definition
 */
static void USBH_UserProcess  (USBH_HandleTypeDef *phost, uint8_t id)
{
  /* USER CODE BEGIN CALL_BACK_1 */
  switch(id)
  {
  case HOST_USER_SELECT_CONFIGURATION:
  break;

  case HOST_USER_DISCONNECTION:

  Appli_state = APPLICATION_DISCONNECT;
  break;

  case HOST_USER_CLASS_ACTIVE:
  Appli_state = APPLICATION_READY;

  // Настроить 115200 8N1 (не обязательно для CC2531, но корректно)
  CDC_LineCodingTypeDef lc;
  lc.b.dwDTERate  = 115200U;  // uint32_t
  lc.b.bCharFormat = 0x00;    // 1 stop bit
  lc.b.bParityType = 0x00;    // none
  USBH_CDC_SetLineCoding(phost, &lc);   // <-- передаём указатель


  HAL_Delay(200); // чуть-чуть дать применить



  // ВАЖНО: поднять линии управления (как делает ПК)
//  USBH_CDC_SetControlLineState(phost, 1/*DTR*/, 1/*RTS*/);
  HAL_Delay(20);

  // Запустить первичный приём
  USBH_CDC_Receive(phost, dummy_rx, sizeof(dummy_rx));

  HAL_GPIO_TogglePin(GPIOC, GPIO_PIN_13);

  break;

  case HOST_USER_CONNECTION:
  Appli_state = APPLICATION_START;
  break;

  default:
  break;
  }
  /* USER CODE END CALL_BACK_1 */
}

/**
  * @}
  */

/**
  * @}
  */

