#define	_FIX_ISSUE_505_
#define	_FIX_ISSUE_181_


#include	<inttypes.h>
#include	<avr/io.h>
//#include	<avr/interrupt.h>
#include	<avr/boot.h>
#include	<avr/pgmspace.h>
//#include	<util/delay.h>
//#include	<avr/eeprom.h>
//#include	<avr/common.h>
#include	"command.h"


/*
 * define CPU frequency in Mhz here if not defined in Makefile
 */
#ifndef F_CPU
	#define F_CPU 16000000UL
#endif

#define	_BLINK_LOOP_COUNT_	(F_CPU / 2250)
/*
 * UART Baudrate, AVRStudio AVRISP only accepts 115200 bps
 */

#ifndef BAUDRATE
	#define BAUDRATE 115200
#endif

/*
 * HW and SW version, reported to AVRISP, must match version of AVRStudio
 */
#define CONFIG_PARAM_BUILD_NUMBER_LOW	0
#define CONFIG_PARAM_BUILD_NUMBER_HIGH	0
#define CONFIG_PARAM_HW_VER				0x0F
#define CONFIG_PARAM_SW_MAJOR			2
#define CONFIG_PARAM_SW_MINOR			0x0A

/*
 * Calculate the address where the bootloader starts from FLASHEND and BOOTSIZE
 * (adjust BOOTSIZE below and BOOTLOADER_ADDRESS in Makefile if you want to change the size of the bootloader)
 */
//#define BOOTSIZE 1024
#if FLASHEND > 0x0F000
	#define BOOTSIZE 8192
#else
	#define BOOTSIZE 2048
#endif

//#define APP_END  (FLASHEND -(2*BOOTSIZE) + 1)
#define APP_END  (FLASHEND -(BOOTSIZE) + 1)

/*
 * Signature bytes are not available in avr-gcc io_xxx.h
 */
#define SIGNATURE_BYTES 0x1E9801


/* ATMega with two USART, use UART0 */
#define	UART_BAUD_RATE_LOW			UBRR0L
#define	UART_STATUS_REG				UCSR0A
#define	UART_CONTROL_REG			UCSR0B
#define	UART_ENABLE_TRANSMITTER		TXEN0
#define	UART_ENABLE_RECEIVER		RXEN0
#define	UART_TRANSMIT_COMPLETE		TXC0
#define	UART_RECEIVE_COMPLETE		RXC0
#define	UART_DATA_REG				UDR0
#define	UART_DOUBLE_SPEED			U2X0


//#define UART_BAUD_SELECT(baudRate,xtalCpu) (((float)(xtalCpu))/(((float)(baudRate))*8.0)-1.0+0.5)



#define UART_BAUD_SELECT(baudRate,xtalCpu) (((float)(xtalCpu))/(((float)(baudRate))*8.0)-1.0+0.5)

/*
 * States used in the receive state machine
 */
#define	ST_START		0
#define	ST_GET_SEQ_NUM	1
#define ST_MSG_SIZE_1	2
#define ST_MSG_SIZE_2	3
#define ST_GET_TOKEN	4
#define ST_GET_DATA		5
#define	ST_GET_CHECK	6
#define	ST_PROCESS		7

typedef uint32_t address_t;

/*
 * function prototypes
 */
static void sendchar(char c);


/*
 * since this bootloader is not linked against the avr-gcc crt1 functions,
 * to reduce the code size, we need to provide our own initialization
 */
#include <avr/sfr_defs.h>

//#define	SPH_REG	0x3E
//#define	SPL_REG	0x3D

#define STACK_TOP (RAMEND - 16)


//*****************************************************************************
/*
 * send single byte to USART, wait until transmission is completed
 */
static void sendchar(char c)
{
	UART_DATA_REG	=	c;										// prepare transmission
	while (!(UART_STATUS_REG & (1 << UART_TRANSMIT_COMPLETE)));	// wait until byte sent
	UART_STATUS_REG |= (1 << UART_TRANSMIT_COMPLETE);			// delete TXCflag
}



#define	MAX_TIME_COUNT	(F_CPU >> 1)
//*****************************************************************************
static unsigned char recchar_timeout(void)
{
	uint32_t count = 0;
	while (!(UART_STATUS_REG & (1 << UART_RECEIVE_COMPLETE)))
	{
		// wait for data
		count++;
		if (count > MAX_TIME_COUNT)
		{
		unsigned int	data;
		#if (FLASHEND > 0x10000)
			data	=	pgm_read_word_far(0);	//*	get the first word of the user program
		#else
			data	=	pgm_read_word_near(0);	//*	get the first word of the user program
		#endif
			if (data != 0xffff)					//*	make sure its valid before jumping to it.
			{
				asm volatile(
						"clr	r30		\n\t"
						"clr	r31		\n\t"
						"ijmp	\n\t"
						);
			}
			count	=	0;
		}
	}
	return UART_DATA_REG;
}



//*	for watch dog timer startup
//void (*app_start)(void) = 0x0000;

	unsigned long flashSize = 0; //flash data size in bytes
	unsigned long flashCounter = 0; //flash counter (readed / written bytes)
	address_t flashAddressLast = 0; //last written flash address
	int flashOperation = 0; //current flash operation (0-nothing, 1-write, 2-verify)

#define RAMSIZE        0x2000
#define boot_src_addr  (*((uint32_t*)(RAMSIZE - 16)))
#define boot_dst_addr  (*((uint32_t*)(RAMSIZE - 12)))
#define boot_copy_size (*((uint16_t*)(RAMSIZE - 8)))
#define boot_reserved  (*((uint8_t*)(RAMSIZE - 6)))
#define boot_app_flags (*((uint8_t*)(RAMSIZE - 5)))
#define boot_app_magic (*((uint32_t*)(RAMSIZE - 4)))
#define BOOT_APP_FLG_ERASE 0x01
#define BOOT_APP_FLG_COPY  0x02
#define BOOT_APP_FLG_FLASH 0x04
	
void _delay(void)
{
	for (int i = 0; i < 100; i++)
		asm("nop");
}

//*****************************************************************************
int main(void)
{
	address_t		address			=	0;
	address_t		eraseAddress	=	0;
	unsigned char	msgParseState;
	unsigned int	ii				=	0;
	unsigned char	checksum		=	0;
	unsigned char	seqNum			=	0;
	unsigned int	msgLength		=	0;
	unsigned char	msgBuffer[285];
	unsigned char	c, *p;
	unsigned char   isLeave = 0;

	unsigned long	boot_timeout;
	unsigned long	boot_timer;
	unsigned int	boot_state;


//	uint8_t	mcuStatusReg;
//	mcuStatusReg	=	MCUSR;

	asm("cli");
	asm("wdr");
//	MCUSR	=	0;
	WDTCSR |= _BV(WDCE) | _BV(WDE);
	WDTCSR = 0;
	asm("sei");
	// check if WDT generated the reset, if so, go straight to app
	if (MCUSR & _BV(WDRF))
	{
/*		if (boot_app_magic == 0x55aa55aa)
		{
///			uint16_t tmp_boot_copy_size = boot_copy_size;
///			uint32_t tmp_boot_src_addr = boot_src_addr;

			address = boot_dst_addr;
			address_t pageAddress = address;
			while (boot_copy_size)
			{
				if (boot_app_flags & BOOT_APP_FLG_ERASE)
				{
					boot_page_erase(pageAddress);
					boot_spm_busy_wait();
				}
				pageAddress += SPM_PAGESIZE;
				if ((boot_app_flags & BOOT_APP_FLG_COPY))
				{
					while (boot_copy_size && (address < pageAddress))
					{
						uint16_t word = 0x0000;
						if (boot_app_flags & BOOT_APP_FLG_FLASH)
							word = pgm_read_word_far(boot_src_addr); //from FLASH
						else
							word = *((uint16_t*)boot_src_addr); //from RAM
						boot_page_fill(address, word);
						address	+= 2;
						boot_src_addr += 2;
						if (boot_copy_size > 2)
							boot_copy_size -= 2;
						else
							boot_copy_size = 0;
					}
					boot_page_write(pageAddress - SPM_PAGESIZE);
					boot_spm_busy_wait();
					boot_rww_enable();
				}
				else
				{
					address	+= SPM_PAGESIZE;
					if (boot_copy_size > SPM_PAGESIZE)
						boot_copy_size -= SPM_PAGESIZE;
					else
						boot_copy_size = 0;
				}
			}
///			boot_copy_size = tmp_boot_copy_size;
///			boot_src_addr = tmp_boot_src_addr;

		}*/
		goto exit;
	}


	boot_timer	=	0;
	boot_state	=	0;

	boot_timeout	=	3500000; // 7 seconds , approx 2us per step when optimize "s"

	UART_STATUS_REG		|=	(1 <<UART_DOUBLE_SPEED);
	UART_BAUD_RATE_LOW	=	UART_BAUD_SELECT(BAUDRATE,F_CPU);
	UART_CONTROL_REG	=	(1 << UART_ENABLE_RECEIVER) | (1 << UART_ENABLE_TRANSMITTER);

	asm("nop");			// wait until port has changed


	while (boot_state==0)
	{
		while (!(UART_STATUS_REG & (1 << UART_RECEIVE_COMPLETE)) && (boot_state == 0))		// wait for data
		{
			_delay();
			boot_timer++;
			if (boot_timer > boot_timeout)
				boot_state	=	1; // (after ++ -> boot_state=2 bootloader timeout, jump to main 0x00000 )
		}
		boot_state++; // ( if boot_state=1 bootloader received byte from UART, enter bootloader mode)
	}

	if (boot_state==1)
	{
		//*	main loop
		while (!isLeave)
		{
			/*
			 * Collect received bytes to a complete message
			 */
			msgParseState	=	ST_START;
			while ( msgParseState != ST_PROCESS )
			{
				if (boot_state==1)
				{
					boot_state	=	0;
					c			=	UART_DATA_REG;
				}
				else
				{
				//	c	=	recchar();
					c	=	recchar_timeout();
					
				}


				switch (msgParseState)
				{
					case ST_START:
						if ( c == MESSAGE_START )
						{
							msgParseState	=	ST_GET_SEQ_NUM;
							checksum		=	MESSAGE_START^0;
						}
						break;

					case ST_GET_SEQ_NUM:
						seqNum			=	c;
						msgParseState	=	ST_MSG_SIZE_1;
						checksum		^=	c;
						break;

					case ST_MSG_SIZE_1:
						msgLength		=	c<<8;
						msgParseState	=	ST_MSG_SIZE_2;
						checksum		^=	c;
						break;

					case ST_MSG_SIZE_2:
						msgLength		|=	c;
						msgParseState	=	ST_GET_TOKEN;
						checksum		^=	c;
						break;

					case ST_GET_TOKEN:
						if ( c == TOKEN )
						{
							msgParseState	=	ST_GET_DATA;
							checksum		^=	c;
							ii				=	0;
						}
						else
						{
							msgParseState	=	ST_START;
						}
						break;

					case ST_GET_DATA:
						msgBuffer[ii++]	=	c;
						checksum		^=	c;
						if (ii == msgLength )
						{
							msgParseState	=	ST_GET_CHECK;
						}
						break;

					case ST_GET_CHECK:
						if ( c == checksum )
						{
							msgParseState	=	ST_PROCESS;
						}
						else
						{
							msgParseState	=	ST_START;
						}
						break;
				}	//	switch
			}	//	while(msgParseState)



			/*
			 * Now process the STK500 commands, see Atmel Appnote AVR068
			 */

			switch (msgBuffer[0])
			{
				case CMD_SIGN_ON:
					msgLength		=	11;
					msgBuffer[1] 	=	STATUS_CMD_OK;
					msgBuffer[2] 	=	8;
					msgBuffer[3] 	=	'A';
					msgBuffer[4] 	=	'V';
					msgBuffer[5] 	=	'R';
					msgBuffer[6] 	=	'I';
					msgBuffer[7] 	=	'S';
					msgBuffer[8] 	=	'P';
					msgBuffer[9] 	=	'_';
					msgBuffer[10]	=	'2';
					break;

				case CMD_GET_PARAMETER:
					{
						unsigned char value;

						switch(msgBuffer[1])
						{
						case PARAM_BUILD_NUMBER_LOW:
							value	=	CONFIG_PARAM_BUILD_NUMBER_LOW;
							break;
						case PARAM_BUILD_NUMBER_HIGH:
							value	=	CONFIG_PARAM_BUILD_NUMBER_HIGH;
							break;
						case PARAM_HW_VER:
							value	=	CONFIG_PARAM_HW_VER;
							break;
						case PARAM_SW_MAJOR:
							value	=	CONFIG_PARAM_SW_MAJOR;
							break;
						case PARAM_SW_MINOR:
							value	=	CONFIG_PARAM_SW_MINOR;
							break;
						default:
							value	=	0;
							break;
						}
						msgLength		=	3;
						msgBuffer[1]	=	STATUS_CMD_OK;
						msgBuffer[2]	=	value;
					}
					break;

				case CMD_LEAVE_PROGMODE_ISP:
					isLeave	=	1;
					//*	fall thru

				case CMD_SET_PARAMETER:
				case CMD_ENTER_PROGMODE_ISP:
					msgLength		=	2;
					msgBuffer[1]	=	STATUS_CMD_OK;
					break;

				case CMD_READ_SIGNATURE_ISP:
					{
						unsigned char signatureIndex	=	msgBuffer[4];
						unsigned char signature;

						if ( signatureIndex == 0 )
							signature	=	(SIGNATURE_BYTES >>16) & 0x000000FF;
						else if ( signatureIndex == 1 )
							signature	=	(SIGNATURE_BYTES >> 8) & 0x000000FF;
						else
							signature	=	SIGNATURE_BYTES & 0x000000FF;

						msgLength		=	4;
						msgBuffer[1]	=	STATUS_CMD_OK;
						msgBuffer[2]	=	signature;
						msgBuffer[3]	=	STATUS_CMD_OK;
					}
					break;

				case CMD_READ_LOCK_ISP:
					msgLength		=	4;
					msgBuffer[1]	=	STATUS_CMD_OK;
					msgBuffer[2]	=	boot_lock_fuse_bits_get( GET_LOCK_BITS );
					msgBuffer[3]	=	STATUS_CMD_OK;
					break;

				case CMD_READ_FUSE_ISP:
					{
						unsigned char fuseBits;

						if ( msgBuffer[2] == 0x50 )
						{
							if ( msgBuffer[3] == 0x08 )
								fuseBits	=	boot_lock_fuse_bits_get( GET_EXTENDED_FUSE_BITS );
							else
								fuseBits	=	boot_lock_fuse_bits_get( GET_LOW_FUSE_BITS );
						}
						else
						{
							fuseBits	=	boot_lock_fuse_bits_get( GET_HIGH_FUSE_BITS );
						}
						msgLength		=	4;
						msgBuffer[1]	=	STATUS_CMD_OK;
						msgBuffer[2]	=	fuseBits;
						msgBuffer[3]	=	STATUS_CMD_OK;
					}
					break;

				case CMD_CHIP_ERASE_ISP:
					eraseAddress	=	0;
					msgLength		=	2;
				//	msgBuffer[1]	=	STATUS_CMD_OK;
					msgBuffer[1]	=	STATUS_CMD_FAILED;	//*	isue 543, return FAILED instead of OK
					break;

				case CMD_LOAD_ADDRESS:
					address	=	( ((address_t)(msgBuffer[1])<<24)|((address_t)(msgBuffer[2])<<16)|((address_t)(msgBuffer[3])<<8)|(msgBuffer[4]) )<<1;
					///address	=	( ((msgBuffer[3])<<8)|(msgBuffer[4]) )<<1;		//convert word to byte address
					msgLength		=	2;
					msgBuffer[1]	=	STATUS_CMD_OK;
					break;
/*
				case CMD_SET_UPLOAD_SIZE_PRUSA3D:
					((unsigned char*)&flashSize)[0] = msgBuffer[1];
					((unsigned char*)&flashSize)[1] = msgBuffer[2];
					((unsigned char*)&flashSize)[2] = msgBuffer[3];
					((unsigned char*)&flashSize)[3] = 0;
					msgLength		=	2;
					msgBuffer[1]	=	STATUS_CMD_OK;
					break;
*/
				case CMD_PROGRAM_FLASH_ISP:
				case CMD_PROGRAM_EEPROM_ISP:
					{
						unsigned int	size	=	((msgBuffer[1])<<8) | msgBuffer[2];
						unsigned char	*p	=	msgBuffer+10;
						unsigned int	data;
						unsigned char	highByte, lowByte;
						address_t		tempaddress	=	address;


						if ( msgBuffer[0] == CMD_PROGRAM_FLASH_ISP )
						{
							if (flashSize != 0)
							{
								if (address == 0) //first page
								{
									flashCounter = size; //initial value = size
									flashAddressLast = 0; //last 
									flashOperation = 1; //write
								}
								else if (address != flashAddressLast)
									flashCounter += size; //add size to counter
								flashAddressLast = address;
							}

							// erase only main section (bootloader protection)
							if (eraseAddress < APP_END ) //erase and write only blocks with address less 0x3e000
							{ //because prevent "brick"
									boot_page_erase(eraseAddress);	// Perform page erase
									boot_spm_busy_wait();		// Wait until the memory is erased.
									eraseAddress += SPM_PAGESIZE;	// point to next page to be erase
							}
							if (address < APP_END)
							{
								/* Write FLASH */
								do {
									lowByte		=	*p++;
									highByte 	=	*p++;

									data		=	(highByte << 8) | lowByte;
									boot_page_fill(address,data);

									address	=	address + 2;	// Select next word in memory
									size	-=	2;				// Reduce number of bytes to write by two
								} while (size);					// Loop until all bytes written

								boot_page_write(tempaddress);
								boot_spm_busy_wait();
								boot_rww_enable();				// Re-enable the RWW section
							}
						}
						/*else
						{
							//	issue 543, this should work, It has not been tested.
							uint16_t ii = address >> 1;
							// write EEPROM
							while (size) {
								eeprom_write_byte((uint8_t*)ii, *p++);
								address+=2;						// Select next EEPROM byte
								ii++;
								size--;
							}
						}*/
						msgLength		=	2;
						msgBuffer[1]	=	STATUS_CMD_OK;

					}
					break;
#if 0
				case CMD_READ_FLASH_ISP:
				case CMD_READ_EEPROM_ISP:
					{
						unsigned int	size	=	((msgBuffer[1])<<8) | msgBuffer[2];
						unsigned char	*p		=	msgBuffer+1;
						msgLength				=	size+3;

						*p++	=	STATUS_CMD_OK;
						if (msgBuffer[0] == CMD_READ_FLASH_ISP )
						{
							if (flashSize != 0)
							{
								if ((address == 0x00000) && (flashOperation == 1))
								{
									flashOperation = 2; //verify
									flashCounter = size; //initial value = size
								}
								else
									flashCounter += size; //add size to counter
							}

							unsigned int data;

							// Read FLASH
							do {
						//#if defined(RAMPZ)
						#if (FLASHEND > 0x10000)
								data	=	pgm_read_word_far(address);
						#else
								data	=	pgm_read_word_near(address);
						#endif
								*p++	=	(unsigned char)data;		//LSB
								*p++	=	(unsigned char)(data >> 8);	//MSB
								address	+=	2;							// Select next word in memory
								size	-=	2;
							}while (size);
						}
						/*else
						{
							// Read EEPROM
							do {
								EEARL	=	address;			// Setup EEPROM address
								EEARH	=	((address >> 8));
								address++;					// Select next EEPROM byte
								EECR	|=	(1<<EERE);			// Read EEPROM
								*p++	=	EEDR;				// Send EEPROM data
								size--;
							} while (size);
						}*/
						*p++	=	STATUS_CMD_OK;
					}
					break;
#endif
				default:
					msgLength		=	2;
					msgBuffer[1]	=	STATUS_CMD_FAILED;
					break;
			}

			/*
			 * Now send answer message back
			 */
			sendchar(MESSAGE_START);
			checksum	=	MESSAGE_START^0;

			sendchar(seqNum);
			checksum	^=	seqNum;

			c			=	((msgLength>>8)&0xFF);
			sendchar(c);
			checksum	^=	c;

			c			=	msgLength&0x00FF;
			sendchar(c);
			checksum ^= c;
			sendchar(TOKEN);
			checksum ^= TOKEN;
			p	=	msgBuffer;
			while ( msgLength )
			{
				c	=	*p++;
				sendchar(c);
				checksum ^=c;
				msgLength--;
			}
			sendchar(checksum);
			seqNum++;
		}
	}

exit:
	asm("nop");                // wait until port has changed
	UART_STATUS_REG	&=	0xfd;
	boot_rww_enable();         // enable application section
	asm("clr r30");
	asm("clr r31");
	asm("ijmp");               // jmp to 0x00000
}
