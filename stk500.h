#ifndef _STK500_H
#define _STK500_H

#include <inttypes.h>


uint8_t stk500_running;

extern int stk500_rxmsg(void);
extern void stk500_txmsg(void);
extern void stk500_command(void);


#endif //_STK500_H
