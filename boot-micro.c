//boot-micro.c

#include <inttypes.h>
#include <avr/io.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include <avr/sfr_defs.h>
#include "command.h"


#define F_CPU 16000000UL

#define BAUDRATE 115200

/*
 * HW and SW version, reported to AVRISP, must match version of AVRStudio
 */
#define CONFIG_PARAM_BUILD_NUMBER_LOW   0
#define CONFIG_PARAM_BUILD_NUMBER_HIGH  0
#define CONFIG_PARAM_HW_VER             0x0F
#define CONFIG_PARAM_SW_MAJOR           2
#define CONFIG_PARAM_SW_MINOR           0x0A

#define FLASHEND 0x3E000

#define BOOTSIZE 8192

#define APP_END  (FLASHEND -(BOOTSIZE) + 1)

#define SIGNATURE_BYTES 0x1E9801

#define UART_BAUD_SELECT(baudRate,xtalCpu) (((float)(xtalCpu))/(((float)(baudRate))*8.0)-1.0+0.5)

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
	

/*
 * States used in the receive state machine
 */
#define ST_START        0
#define ST_GET_SEQ_NUM  1
#define ST_MSG_SIZE_1   2
#define ST_MSG_SIZE_2   3
#define ST_GET_TOKEN    4
#define ST_GET_DATA     5
#define ST_GET_CHECK    6
#define ST_PROCESS      7

typedef uint32_t address_t;


void sendchar(char c)
{
	UDR0 = c;                         // prepare transmission
	while (!(UCSR0A & (1 << TXC0)));  // wait until byte sent
	UCSR0A |= (1 << TXC0);            // delete TXCflag
}

#define	MAX_TIME_COUNT	(F_CPU >> 1)
//*****************************************************************************
unsigned char recchar_timeout(void)
{
	uint32_t count = 0;
	while (!(UCSR0A & (1 << RXC0)))
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
	return UDR0;
}




void _delay(void)
{
	for (int i = 0; i < 100; i++)
		asm("nop");
}

const PROGMEM uint8_t signature[3] =
{
	(SIGNATURE_BYTES >> 16) & 0xff,
	(SIGNATURE_BYTES >> 8) & 0xff,
	SIGNATURE_BYTES & 0xff,
};

uint8_t param(uint8_t param)
{
	switch (param)
	{
	case PARAM_BUILD_NUMBER_LOW: return CONFIG_PARAM_BUILD_NUMBER_LOW;
	case PARAM_BUILD_NUMBER_HIGH: return CONFIG_PARAM_BUILD_NUMBER_HIGH;
	case PARAM_HW_VER: return CONFIG_PARAM_HW_VER;
	case PARAM_SW_MAJOR: return CONFIG_PARAM_SW_MAJOR;
	case PARAM_SW_MINOR: return CONFIG_PARAM_SW_MINOR;
	}
	return 0;
};


int main(void)
{
	address_t address = 0;
	address_t eraseAddress = 0;
	unsigned char msgParseState;
	unsigned int ii = 0;
	unsigned char checksum = 0;
	unsigned char seqNum = 0;
	unsigned int msgLength = 0;
	unsigned char msgBuffer[285];
	unsigned char c, *p;
	unsigned char isLeave = 0;

	unsigned long boot_timer;
	unsigned int boot_state;

	asm("cli");
	asm("wdr");
//	MCUSR	=	0;
	WDTCSR |= _BV(WDCE) | _BV(WDE);
	WDTCSR = 0;
	asm("sei");
	// check if WDT generated the reset, if so, go straight to app
	if (MCUSR & _BV(WDRF))
		goto exit;

	boot_timer = 35000; // 2s
	boot_state = 0;

	UCSR0A |= (1 <<U2X0);
	UBRR0L = UART_BAUD_SELECT(BAUDRATE,F_CPU);
	UCSR0B = (1 << RXEN0) | (1 << TXEN0);

	asm("nop"); // wait until port has changed

	while (!(UCSR0A & (1 << RXC0)) && (boot_timer--)) // wait for data
		_delay();
	if (boot_timer == 0) goto exit;
	boot_state = 1;
	while (!isLeave)
	{
		msgParseState = ST_START;
		ii = 0;
		while (msgParseState != ST_PROCESS)
		{
			boot_timer = 100000;
			while (!(UCSR0A & (1 << RXC0)) && (boot_timer--)) // wait for data
			if (boot_timer == 0) goto exit;
			c = UDR0;
			checksum ^= c;
			switch (msgParseState)
			{
			case ST_START:
				if (c == MESSAGE_START)
				{
					msgParseState = ST_GET_SEQ_NUM;
					checksum = MESSAGE_START;
				}
				break;
			case ST_GET_SEQ_NUM:
				seqNum = c;
				msgParseState = ST_MSG_SIZE_1;
				break;
			case ST_MSG_SIZE_1:
				msgLength = c << 8;
				msgParseState = ST_MSG_SIZE_2;
				break;
			case ST_MSG_SIZE_2:
				msgLength |= c;
				msgParseState = ST_GET_TOKEN;
				break;
			case ST_GET_TOKEN:
				if (c == TOKEN)
					msgParseState = ST_GET_DATA;
				else
					msgParseState = ST_START;
				break;
			case ST_GET_DATA:
				msgBuffer[ii++]	= c;
				if (ii == msgLength)
					msgParseState = ST_GET_CHECK;
				break;
			case ST_GET_CHECK:
//				if (c == checksum)
					msgParseState = ST_PROCESS;
//				else
//					msgParseState = ST_START;
				break;
			}
		}

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
			msgLength = 3;
			msgBuffer[2] = param(msgBuffer[1]);
			msgBuffer[1] = STATUS_CMD_OK;
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
			msgLength =	4;
			msgBuffer[1] = STATUS_CMD_OK;
			msgBuffer[2] = pgm_read_byte_far(signature + msgBuffer[4]); //msgBuffer[4] is signatureIndex
			msgBuffer[3] = STATUS_CMD_OK;
			break;
		case CMD_LOAD_ADDRESS:
			address	=	( ((address_t)(msgBuffer[1])<<24)|((address_t)(msgBuffer[2])<<16)|((address_t)(msgBuffer[3])<<8)|(msgBuffer[4]) )<<1;
			///address	=	( ((msgBuffer[3])<<8)|(msgBuffer[4]) )<<1;		//convert word to byte address
			msgLength		=	2;
			msgBuffer[1]	=	STATUS_CMD_OK;
			break;
		case CMD_PROGRAM_FLASH_ISP:
			{
				unsigned int size = ((msgBuffer[1])<<8) | msgBuffer[2];
				unsigned char *p = msgBuffer+10;
				unsigned int data;
				unsigned char highByte, lowByte;
				address_t tempaddress = address;
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
				msgLength		=	2;
				msgBuffer[1]	=	STATUS_CMD_OK;
			}
			break;
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

exit:
	asm("nop");                // wait until port has changed
	UCSR0A	&=	0xfd;
	boot_rww_enable();         // enable application section
	asm("clr r30");
	asm("clr r31");
	asm("ijmp");               // jmp to 0x00000
}
