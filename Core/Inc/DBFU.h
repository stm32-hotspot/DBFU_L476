/*
 * dbfu.h
 *
 *  Created on: Dec 12, 2022
 *      Author: Troy Davis
 */


#ifndef INC_DBFU_H_
#define INC_DBFU_H_

typedef enum
{
  DBFU_OK,
  DBFU_CRC_Error,
  DBFU_BankSwitch_Error,
  DBFU_FlashErase_Error,
  DBFU_FlashWrite_Error,
  DBFU_UART_Error,
	DBFU_Initiation_Error
} DBFU_ErrorTypeDef;

DBFU_ErrorTypeDef DBFU_Process(void);

#endif /* INC_DBFU_H_ */
