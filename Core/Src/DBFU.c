/*
 * DBFU.c
 *
 *  Created on: Dec 12, 2022
 *      Author: Troy Davis
 */

#include <DBFU.h>
#include "main.h"
#include <string.h>
#include <stdio.h>

/*
 * DBFU Packet header byte definitions, inspired by YMODEM packet header definitions
 */
#define ACK 0x06
#define EOT 0x54
#define SOH 0x01
#define START 'S'

/*
 * DMA buffer handles 1024B data, plus 16 bit checksum
 */
#define DMA_BUFF_LENGTH 1026
#define PACKET_LENGTH 1024			// Actual binary data packet comes in chunks of 1024 bytes
#define BANK_ADDRESS 0x8080000	// Address of other bank
#define PAGE_SIZE 2048					//Page size in bytes for calculating


/*
 * Extern variable peripheral handles
 */
extern UART_HandleTypeDef huart2;
extern DMA_HandleTypeDef hdma_usart2_rx;
extern CRC_HandleTypeDef hcrc;
extern uint32_t current_bank;

uint8_t num_pages_to_erase;
uint8_t dbfu_dma_buff[DMA_BUFF_LENGTH];					// Packet of binary file data copied from DAM buffer
uint32_t program_address = BANK_ADDRESS;	// Other bank address for Flash Program
uint32_t doubleword_data_idx = 0;

/*
 * Booleans for handling Flash ISR timing
 */
bool flash_write_done = true;		// Single doubleword is written
bool flashing_ongoing = false;	// 1024 byte packet is written
bool erasing_ongoing = false;		// pages are still erasing

typedef enum
{
	idle,
  starting,
	receiving_packet,
	packet_received,
	flashing,
	finishing
}DBFUStatusEnum;

DBFUStatusEnum DBFUStatus = starting;


/*
 * ISR Callbacks
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	/*
	 * If UART DMA reception is complete, then stop the DMA transfer to allow for next
	 * packet reception and set SBFU Status accordingly
	 */
  HAL_UART_DMAStop(&huart2);
  DBFUStatus = packet_received;
}

void HAL_FLASH_EndOfOperationCallback(uint32_t ReturnValue)
{
	// EndOfOperation callback is used by both erasing and writing, so check current operation
  if (erasing_ongoing)
  {
    // ReturnValue = 0xFFFFFFFF is how the ISR indicates that it is done erasing pages
    if (ReturnValue == 0xFFFFFFFF)
    	erasing_ongoing = false;
  }
  else if (DBFUStatus == flashing)
  {
    // Bool to ensure that we don't call HAL_FLASH_Program_IT again before operation is finished
    flash_write_done = true;
  }
}


static DBFU_ErrorTypeDef Swap_Banks(void)
{
  __disable_irq();
  /* Clear OPTVERR bit set on virgin samples */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_OPTVERR);

  FLASH_OBProgramInitTypeDef OB;
  HAL_FLASHEx_OBGetConfig(&OB);

  HAL_FLASH_Unlock();
  HAL_FLASH_OB_Unlock();

  OB.OptionType = OPTIONBYTE_USER;
  OB.USERType = OB_USER_BFB2;

  // Toggle the BFB2 bit
  if (OB.USERConfig & FLASH_OPTR_BFB2)
  {
    OB.USERConfig = OB_BFB2_DISABLE;
  }
  else
  {
    OB.USERConfig = OB_BFB2_ENABLE;
  }

  /*
   * Attempt option byte programming and launching
   */
  if ( HAL_FLASHEx_OBProgram(&OB) == HAL_OK )
  {
    if ( HAL_FLASH_OB_Launch()!= HAL_OK )
    {
      // Should never make it here
      HAL_FLASH_OB_Lock();
      HAL_FLASH_Lock();
      return DBFU_BankSwitch_Error;
    }
  }
  // Should never make it here
  HAL_FLASH_OB_Lock();
  HAL_FLASH_Lock();
  return DBFU_BankSwitch_Error;

}

static DBFU_ErrorTypeDef Get_Filesize_Erase_Pages()
{
#ifdef USE_TRACE
  printf("Executing Get_Filesize_Erase_Pages()\n");
#endif

  /*
   * Filesize is received as a 3 byte packet after script receives START
   */
  uint32_t filesize;
  uint8_t filesize_bytes[3];
  uint8_t byte_tx = START;
  // Indicate to loader script that we want to start the update
  if(HAL_UART_Transmit(&huart2, &byte_tx, 1, 0xFFFF) != HAL_OK)
    return DBFU_UART_Error;

  /*
   * If script doesn't respond to start, return initiation error
   * Either there is a hardware communication issue, or more likely
   * that the script hasn't been properly started yet.
   *
   * Timeout = 100ms
   */
  if(HAL_UART_Receive(&huart2, filesize_bytes, 3, 100) != HAL_OK)
    return DBFU_Initiation_Error;

  // Obtain file size from 3 byte packet
  filesize = ( filesize_bytes[0] << 16 ) | (filesize_bytes[1] << 8) | (filesize_bytes[2]);

  // Calculate number of pages to erase
  num_pages_to_erase = (filesize / PAGE_SIZE) + 1;

  FLASH_EraseInitTypeDef EraseInit =
  {
      .Banks = current_bank == 1 ? FLASH_BANK_2 : FLASH_BANK_1,
      .TypeErase = FLASH_TYPEERASE_PAGES,
      .Page = 256,
      .NbPages = num_pages_to_erase
  };

  HAL_FLASH_Unlock();

	erasing_ongoing = true;
  if ( HAL_FLASHEx_Erase_IT(&EraseInit) != HAL_OK)
    return DBFU_FlashErase_Error;

  DBFUStatus = receiving_packet;
  return DBFU_OK;
}


static DBFU_ErrorTypeDef Handle_Packet()
{
#ifdef USE_TRACE
  printf("Executing Handle_Packet()\n");
#endif

  /*
   * There is a possibility that the flash ISR flag isn't true, so make sure it
   * is set true here
   */
  flash_write_done = true;
  uint8_t byte_tx = ACK;
  uint8_t packet_header;

  // Send ACK to script
  if(HAL_UART_Transmit(&huart2, &byte_tx, 1, 0xFFFF) != HAL_OK)
    return DBFU_UART_Error;

  // Receive packet header byte
  if(HAL_UART_Receive(&huart2, &packet_header, 1, 0xFFFF) != HAL_OK)
    return DBFU_UART_Error;

  // Start receiving packet of data with DMA
  if(HAL_UART_Receive_DMA(&huart2, dbfu_dma_buff, DMA_BUFF_LENGTH) != HAL_OK)
    return DBFU_UART_Error;

  /*
   * If packet header is EOT, then we will stop the DMA transfer
   * as file reception is already complete. Setting the DBFUStatus to
   * finishing allows flash write of the previous packet to finish
   * before swapping banks
   */
  if (packet_header == EOT)
  {
    HAL_UART_DMAStop(&huart2);
    DBFUStatus = finishing;
  }
  else
  {
  	DBFUStatus = idle;
  }

  return DBFU_OK;
}

/*
 * Copies data from DMA packet and uses CRC to verify data integrity
 */
static DBFU_ErrorTypeDef Do_CRC()
{
#ifdef USE_TRACE
  printf("Executing Copy_Data_Do_CRC()\n");
#endif
  // Get expected 16-bit checksum out of last two bytes of DMA buffer
  uint16_t expected_checksum =   (dbfu_dma_buff[DMA_BUFF_LENGTH-2] << 8) |
                                  dbfu_dma_buff[DMA_BUFF_LENGTH-1];

  // Calculate the checksum of the data
  uint16_t calculated_checksum = HAL_CRC_Calculate(&hcrc, (uint32_t *)dbfu_dma_buff, PACKET_LENGTH);

  // Make sure the checksums match
  if (expected_checksum != calculated_checksum)
  {
    return DBFU_CRC_Error;
  }

  DBFUStatus = flashing;
  flashing_ongoing = true;
  return DBFU_OK;
}


static DBFU_ErrorTypeDef Handle_Flashing()
{
  uint64_t flash_doubleword = *((uint64_t *)&dbfu_dma_buff[doubleword_data_idx]);

	while (!flash_write_done);
  flash_write_done = false;
	if(HAL_FLASH_Program_IT(FLASH_TYPEPROGRAM_DOUBLEWORD, program_address, flash_doubleword) != HAL_OK)
		return DBFU_FlashWrite_Error;

	/*
	 * Iterating through 64 bit data
	 */
  program_address += 8;
  doubleword_data_idx += 8;

  // Packet has been fully written
  if(doubleword_data_idx == PACKET_LENGTH)
  {
  	doubleword_data_idx = 0;
  	flashing_ongoing = false;
  	DBFUStatus = receiving_packet;
#ifdef USE_TRACE
    printf("Chunk Written\n");
#endif
  }

  return DBFU_OK;
}


/*
 * Main processing function for DBFU
 */
DBFU_ErrorTypeDef DBFU_Process(void)
{

  DBFU_ErrorTypeDef error_code = DBFU_OK;

  switch (DBFUStatus)
	{
  	/*
  	 * Case starting:
  	 * Receive filesize bytes and erase needed amount of pages
  	 */
  	case starting:
      error_code = Get_Filesize_Erase_Pages();
      if (error_code != DBFU_OK)
           return error_code;
  		break;

		/*
		 * Case receiving_packet:
		 * Handle the packet header byte and start DMA reception of binary data
		 */
  	case receiving_packet:
      error_code = Handle_Packet();
      if (error_code != DBFU_OK)
        return error_code;
  		break;

		/*
		 * Case packet_received:
		 * Perform CRC on app binary data
		 */
  	case packet_received:
      error_code = Do_CRC();
      if (error_code != DBFU_OK)
        return error_code;
  		break;

		/*
		 * Case flashing:
		 * Write bytes to flash 64 bits at a time for each call.
		 * Makes sure pages aren't still being erased
		 */
  	case flashing:
  		if (!erasing_ongoing)
  		{
  			error_code = Handle_Flashing();
        if (error_code != DBFU_OK)
          return error_code;
  		}
  		break;

  		/*
  		 * Case finishing
  		 * end of transfer received, last packet has been sent
  		 * make sure we have finished flashing last data packet
  		 */
  	case finishing:
  		if (!flashing_ongoing)
  		{
				error_code = Swap_Banks();
				// Function should never return, so always return error code
				return error_code;
				break;
  		}

  	case idle:
  		break;
	}

  return error_code;
}
