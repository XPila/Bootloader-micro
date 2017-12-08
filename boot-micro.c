//boot-micro.c

#include <inttypes.h>
#include <avr/io.h>
#include <avr/boot.h>
#include <avr/pgmspace.h>
#include <avr/sfr_defs.h>
#include "stk500.h"


#define F_CPU 16000000UL

#define BAUDRATE 115200

#define UART_BAUD_SELECT(baudRate,xtalCpu) (((float)(xtalCpu))/(((float)(baudRate))*8.0)-1.0+0.5)



int main(void)
{
	asm("cli");
	asm("wdr");
//	MCUSR	=	0;
	WDTCSR |= _BV(WDCE) | _BV(WDE);
	WDTCSR = 0;
	asm("sei");
	// check if WDT generated the reset, if so, go straight to app
	if (MCUSR & _BV(WDRF)) goto exit;

	UCSR0A |= (1 <<U2X0);
	UBRR0L = UART_BAUD_SELECT(BAUDRATE,F_CPU);
	UCSR0B = (1 << RXEN0) | (1 << TXEN0);
	asm("nop"); // wait until port has changed

	stk500_running = 1;
	while (stk500_running)
	{
		if (stk500_rxmsg() < 0) goto exit;
		stk500_command();
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
