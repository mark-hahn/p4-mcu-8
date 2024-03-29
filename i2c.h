#ifndef I2C_H
#define	I2C_H

#include <xc.h>
#include "types.h"
#include "motor.h"

#define WCOL  SSP1CON1bits.WCOL
#define SSPOV SSP1CON1bits.SSPOV
#define CKP   SSP1CON1bits.CKP
#define SSPEN SSP1CON1bits.SSPEN

#define NUM_RECV_BYTES NUM_SETTING_WORDS*2+2
#define NUM_SEND_BYTES 3  //  state, posH, posL

#define I2C_ADDR_MASK 0xf8  // motor idx in d2-d1 (d1-d0 in real addr)
#define I2C_ADDR      0x20  // real addr: 0x10 + motor #

extern volatile uint8 i2cRecvBytes[NUM_MOTORS][NUM_RECV_BYTES];

void i2cInit(void);
void checkI2c(void);
void i2cInterrupt(void);

#endif	/* I2C_H */
