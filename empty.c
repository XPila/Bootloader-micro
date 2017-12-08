//empty.c

#include <inttypes.h>
#include <avr/io.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include <avr/sfr_defs.h>
#include "command.h"

#define ST_START        0
#define ST_GET_SEQ_NUM  1
#define ST_MSG_SIZE_1   2
#define ST_MSG_SIZE_2   3
#define ST_GET_TOKEN    4
#define ST_GET_DATA     5
#define ST_GET_CHECK    6
#define ST_PROCESS      7

void _delay(void);
void uart_tx(uint8_t c);
void stk500_rxmsg(void);


//empty project - 292 bytes
//+exit section - 318 bytes
//+_delay - 334
//+uart_tx - 358

int main(void)
{
exit: //exit section - 26 bytes
	asm("nop");                // wait until port has changed
	UCSR0A	&=	0xfd;
	boot_rww_enable();         // enable application section
	asm("clr r30");
	asm("clr r31");
	asm("ijmp");               // jmp to 0x00000
}

void _delay(void) //16 bytes
{
	for (int i = 0; i < 100; i++)
		asm("nop");
}

uint8_t stk500_checksum;
uint8_t stk500_seq_num;
uint16_t stk500_length;

void stk500_tx(uint8_t c) //24 bytes
{
	UDR0 = c;                         // prepare transmission
	while (!(UCSR0A & (1 << TXC0)));  // wait until byte sent
	UCSR0A |= (1 << TXC0);            // delete TXCflag
	stk500_checksum ^= c;
}

int stk500_rx() //24 bytes
{
	uint8_t timer = 18000; // 1s
	while (!(UCSR0A & (1 << RXC0)) && (--timer)) _delay(); // wait for data
	if (timer == 0) return -1;
	uint8_t c = UDR;
	stk500_checksum ^= c;
	return c;
}

void stk500_rxmsg(void)
{
	uint8_t state = ST_START;
	uint8_t ii = 0;
	while (state != ST_PROCESS)
	{
		timer = 18000; // 1s
		while (!(UCSR0A & (1 << RXC0)) && (--timer)) _delay(); // wait for data
		if (timer == 0) goto exit;
		c = UDR0;
		stk500_checksum ^= c;
		switch (state)
		{
		case ST_START:
			if (c == MESSAGE_START)
			{
				state = ST_GET_SEQ_NUM;
				stk500_checksum = MESSAGE_START;
			}
			break;
		case ST_GET_SEQ_NUM:
			stk500_seq_num = c;
			state = ST_MSG_SIZE_1;
			break;
		case ST_MSG_SIZE_1:
			stk500_length = c << 8;
			state = ST_MSG_SIZE_2;
			break;
		case ST_MSG_SIZE_2:
			stk500_length |= c;
			state = ST_GET_TOKEN;
			break;
		case ST_GET_TOKEN:
			if (c == TOKEN)
				state = ST_GET_DATA;
			else
				state = ST_START;
			break;
		case ST_GET_DATA:
			msgBuffer[ii++]	= c;
			if (ii == stk500_length)
				state = ST_GET_CHECK;
			break;
		case ST_GET_CHECK:
			if (stk500_checksum == 0)
				state = ST_PROCESS;
			else
				state = ST_START;
			break;
		}
	}
}
