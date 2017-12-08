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

//#pragma pack(push)
//#pragma pack(1)
/*typedef struct
{
	uint8_t start;
	uint8_t seq_num;
	uint16_t length;
	uint8_t token;
	uint8_t data[];
} stk500_msg_t;*/
//#pragma pack(pop)

uint8_t stk500_checksum;
uint8_t stk500_seq_num;
uint16_t stk500_length;
uint8_t stk500_buffer[285];

void stk500_tx(uint8_t c)
{
	UDR0 = c;                         // prepare transmission
	while (!(UCSR0A & (1 << TXC0)));  // wait until byte sent
	UCSR0A |= (1 << TXC0);            // delete TXCflag
	stk500_checksum ^= c;             // update checksum
}


/*int stk500_rxmsg()
{
}
*/
void stk500_txmsg(void)
{
	stk500_checksum = 0x00;
	stk500_tx(MESSAGE_START);
	stk500_tx(stk500_seq_num);
	stk500_tx((stk500_length >> 8) & 0xff);
	stk500_tx(stk500_length & 0xff);
	stk500_tx(TOKEN);
	uint8_t* p = stk500_buffer;
	while (stk500_length--)
		stk500_tx(*(p++));
	stk500_tx(stk500_checksum);
	stk500_seq_num++;
}

int main(void)
{
	address_t address = 0;
	address_t eraseAddress = 0;
	uint8_t msgParseState;
	uint16_t ii = 0;
	register uint8_t c;
	uint8_t *p;
	uint8_t isLeave = 0;

	uint16_t timer;

	asm("cli");
	asm("wdr");
//	MCUSR	=	0;
	WDTCSR |= _BV(WDCE) | _BV(WDE);
	WDTCSR = 0;
	asm("sei");
	// check if WDT generated the reset, if so, go straight to app
	if (MCUSR & _BV(WDRF))
		goto exit;


	UCSR0A |= (1 <<U2X0);
	UBRR0L = UART_BAUD_SELECT(BAUDRATE,F_CPU);
	UCSR0B = (1 << RXEN0) | (1 << TXEN0);

	asm("nop"); // wait until port has changed

//	timer = 18000; // 1s
//	while (!(UCSR0A & (1 << RXC0)) && (--timer)) _delay(); // wait for data
//	if (timer == 0) goto exit;

	while (!isLeave)
	{
		msgParseState = ST_START;
		ii = 0;
		while (msgParseState != ST_PROCESS)
		{
			timer = 18000; // 1s
			while (!(UCSR0A & (1 << RXC0)) && (--timer)) _delay(); // wait for data
			if (timer == 0) goto exit;
			c = UDR0;
			stk500_checksum ^= c;
			switch (msgParseState)
			{
			case ST_START:
				if (c == MESSAGE_START)
				{
					msgParseState = ST_GET_SEQ_NUM;
					stk500_checksum = MESSAGE_START;
				}
				break;
			case ST_GET_SEQ_NUM:
				stk500_seq_num = c;
				msgParseState = ST_MSG_SIZE_1;
				break;
			case ST_MSG_SIZE_1:
				stk500_length = c << 8;
				msgParseState = ST_MSG_SIZE_2;
				break;
			case ST_MSG_SIZE_2:
				stk500_length |= c;
				msgParseState = ST_GET_TOKEN;
				break;
			case ST_GET_TOKEN:
				if (c == TOKEN)
					msgParseState = ST_GET_DATA;
				else
					msgParseState = ST_START;
				break;
			case ST_GET_DATA:
				stk500_buffer[ii++]	= c;
				if (ii == stk500_length)
					msgParseState = ST_GET_CHECK;
				break;
			case ST_GET_CHECK:
				if (stk500_checksum == 0)
					msgParseState = ST_PROCESS;
				else
					msgParseState = ST_START;
				break;
			}
		}

		switch (stk500_buffer[0])
		{
		case CMD_SIGN_ON:
			stk500_length = 11;
			stk500_buffer[1] = STATUS_CMD_OK;
			stk500_buffer[2] = 8;
			stk500_buffer[3] = 'A';
			stk500_buffer[4] = 'V';
			stk500_buffer[5] = 'R';
			stk500_buffer[6] = 'I';
			stk500_buffer[7] = 'S';
			stk500_buffer[8] = 'P';
			stk500_buffer[9] = '_';
			stk500_buffer[10] = '2';
			break;
		case CMD_GET_PARAMETER:
			stk500_length = 3;
			stk500_buffer[2] = param(stk500_buffer[1]);
			stk500_buffer[1] = STATUS_CMD_OK;
			break;
		case CMD_LEAVE_PROGMODE_ISP:
			isLeave	=	1;
			//*	fall thru
		case CMD_SET_PARAMETER:
		case CMD_ENTER_PROGMODE_ISP:
			stk500_length		=	2;
			stk500_buffer[1]	=	STATUS_CMD_OK;
			break;
		case CMD_READ_SIGNATURE_ISP:
			stk500_length =	4;
			stk500_buffer[1] = STATUS_CMD_OK;
			stk500_buffer[2] = pgm_read_byte_far(signature + stk500_buffer[4]); //stk500_buffer[4] is signatureIndex
			stk500_buffer[3] = STATUS_CMD_OK;
			break;
		case CMD_LOAD_ADDRESS:
			address	=	( ((address_t)(stk500_buffer[1])<<24)|((address_t)(stk500_buffer[2])<<16)|((address_t)(stk500_buffer[3])<<8)|(stk500_buffer[4]) )<<1;
			///address	=	( ((stk500_buffer[3])<<8)|(stk500_buffer[4]) )<<1;		//convert word to byte address
			stk500_length		=	2;
			stk500_buffer[1]	=	STATUS_CMD_OK;
			break;
		case CMD_PROGRAM_FLASH_ISP:
			{
				uint16_t size = ((stk500_buffer[1])<<8) | stk500_buffer[2];
				uint8_t *p = stk500_buffer+10;
				uint16_t data;
				uint8_t highByte, lowByte;
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
				stk500_length		=	2;
				stk500_buffer[1]	=	STATUS_CMD_OK;
			}
			break;
		default:
			stk500_length		=	2;
			stk500_buffer[1]	=	STATUS_CMD_FAILED;
			break;
		}
		stk500_txmsg();
	}

exit:
	asm("nop");                // wait until port has changed
	UCSR0A	&=	0xfd;
	boot_rww_enable();         // enable application section
	asm("clr r30");
	asm("clr r31");
	asm("ijmp");               // jmp to 0x00000
}
