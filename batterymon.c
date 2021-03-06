/*
 * File:   batterymon.c
 *
 *
 *
 *  Copyright (C) 2012  Stephen Rodgers
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 *
 * Stephen "Steve" Rodgers <hwstar@rodgers.sdcoxmail.com>
 *
 * Created on July 30, 2012, 7:01PM
 */


#include <xc.h>
#include <stdint.h>
#include "han.h"

__CONFIG(WDTE_OFF & LVP_OFF & FOSC_INTOSC & 
        PWRTE_ON & CP_OFF & CPD_OFF & BOREN_ON & CLKOUTEN_OFF &
        IESO_ON & FCMEN_OFF);

__CONFIG(WRT_ALL & VCOREV_OFF & PLLEN_ON & STVREN_ON &
        BORV_LO & DEBUG_OFF & LVP_OFF);

/* Port definitions */

#define ADDRPROG    PORTAbits.RA0
#define ALERT       PORTAbits.RA2
#define OD1         LATAbits.LATA4
#define OD2         LATAbits.LATA5
#define TXENA       LATCbits.LATC3
#define LED         LATCbits.LATC2

/* MISC Constants */
#define TRUE 1
#define FALSE 0
#define OFF 0
#define ON 1
#define PASS 1
#define FAIL 0
#define NOERR 0
#define ERR 1



/* Oscillator frequency */
#define _XTAL_FREQ 32000000

/* HAN Module ID and firmware version */
#define MODULEID 0x1007     // MODULE ID
#define VERSION  0x0000     // VERSION


/* EEPROM */
#define EECONFIGSTART   0x00
#define EESIG           0x55AA
#define DEF_SHUNT_AMPS  200
#define DEF_SHUNT_MV    50

/* INA226 I2C address */
#define INI226_ADDR 0x80

/* INA226 register pointers */
#define INA226_CONFIG   0x00
#define INA226_SHUNT    0x01
#define INA226_BUS      0x02
#define INA226_POWER    0x03
#define INA226_CURRENT  0x04
#define INA226_CAL      0x05

/* INA226 Initial Constants */
#define INA226_INIT_CONFIG 0x0927

/* Misc constants */
#define VOLTRES 1250       // Microvolt per bit
#define VMAG -6
#define CMAG -7
#define PMAG -3

/* Macros */

#define SET_BAUD(B) (((_XTAL_FREQ/B)/64) - 1)

#define INA226_TRANS_START(RP, RW, REG )\
{i2c.rw = RW; i2c.regptr = RP; i2c.reg = REG; i2c.busy = TRUE; SSP1CON2bits.SEN = TRUE;}

#define INA226_TRANS_BUSY (i2c.busy)

#define INA226_TRANS_WAIT(RP, RW, REG) {INA226_TRANS_START(RP, RW, REG);\
while(INA226_TRANS_BUSY) CLRWDT();}

#define INA226_RESULT i2c.reg

#define ADDRPROGMODE (ADDRPROG == 1) // Jumper removed



/*
 * Derived types
 */

/* I2C States*/
typedef enum {I2C_SEND_ADDR = 0, I2C_SEND_REG,
            I2C_READ_WRITE, I2C_WRITE_LOW,
            I2C_READ_ADDR, I2C_READ_START,
            I2C_READ_HIGH, I2C_READ_HIGH_ACK,
            I2C_READ_LOW,
            I2C_STOP, I2C_DONE} i2cstate_t;

/* I2C Control block */

typedef struct {
    struct {
        unsigned rw : 1;
        unsigned busy : 1;
        unsigned err : 1;
    };
    uint8_t regptr;
    uint8_t reghi;
    uint8_t reglow;
    uint16_t reg;
    struct {
        i2cstate_t state;
    }priv;
}i2c_t;

/* EE Data */
typedef union {
    struct {
        uint16_t sig;
        uint8_t  shunt_mv;
        uint8_t pad1;
        uint16_t shunt_amps;


    };
    uint8_t bytes[16];
} eedata_t;


/*
 * Private variables
 */

#ifdef BOOTAPP
static bit enterbootloader = FALSE;
#endif
static volatile uint8_t ledactivitytimer = 0;
static uint8_t crcreg = 0;
static uint8_t myaddress = 0;
static uint16_t ina226_cal;                     // INA226 calibration constant
static uint32_t current_lsb;                    // current lsb in 10exp-7 amps
static uint32_t power_lsb;                      // power lsb (25x Current lsb)

static volatile rxi_t   rxi;                    // Rcv interrupt handler vars
static volatile pkt_t	pkt;			// Packet
static volatile txi_t	txi;			// Tx interrupt handler vars
static volatile irq_t	irq;			// IRQ variables
static volatile phd_t	phd;			// Packet handler data
static volatile i2c_t   i2c;                    // i2c control block
static eedata_t eedata;                         // copy of EEPROM data in RAM

/*
 * UART receive interrupt service
 */

static void handle_rda()
{
	rxi.c = RCREG;

	irq.timer = irq.holdoff;

	if(TRUE == rxi.pready)
		return;

	if(!rxi.sub){
		if(STX == rxi.c)
			rxi.state = RXI_INIT; // Start from beginning
		else if(ETX == rxi.c){
			rxi.state = RXI_FINISH; // Finish up
		}
		else if(SUBST == rxi.c){
			rxi.sub = TRUE; // Next char is a substitution
			return;
		}
	}



	switch(rxi.state){

		case	RXI_INIT:
                        if(STX == rxi.c){
                            rxi.packettimer = 0xFF;
                            rxi.state = RXI_ASSEM;
                            rxi.index = 0;
                        }
			break;

		case	RXI_ASSEM:
			rxi.sub = FALSE;
			if(rxi.index < MAXPACKET){
				((uint8_t *) &pkt)[rxi.index++] = rxi.c;
			}
			break;


		case	RXI_FINISH:
			rxi.sub = FALSE;
			rxi.state = RXI_INIT;
			rxi.pready = TRUE;
			break;

		default:
			rxi.state = RXI_INIT;
			break;

	}
	rxi.sub = FALSE;

}

/*
 *Uart Transmit interrupt service
 */

static void handle_tbe()
{
    switch(txi.state){
            case TXI_INIT:
                    TXREG = STX; // Send Start of TX
                    txi.state = TXI_TXC;
                    txi.index = 0;
                    break;


            case TXI_TXC:
                    txi.tchar = ((uint8_t *)&pkt)[txi.index++];
                    if(txi.tchar <= SUBST){
                            TXREG = SUBST; // Send SUBST and return.
                            txi.state = TXI_TXC_POSTSUB;
                            return;
                    }
                    // break intentionally left out
            case TXI_TXC_POSTSUB:
                    TXREG = txi.tchar; // Send actual character
                    txi.blen--;
                    if(!txi.blen)
                            txi.state = TXI_FIN;

                    else
                            txi.state = TXI_TXC;
                    break;

            case TXI_FIN:
                    TXREG = ETX; // Send End of TX
                    txi.state = TXI_INIT;
                    PIE1bits.TXIE = FALSE; // Shut off TX interrupt
                    txi.txbusy = FALSE;
                    break;

            default:
                    PIE1bits.TXIE = FALSE; // Shut off TX interrupt
                    txi.state = TXI_INIT;
                    break;
    }
}

/*
 * Timer interrupt service
 */

static void handle_timer0() // 1.024 mSec
{
    // Packet time out timer
    if(RXI_ASSEM == rxi.state){
        if(!rxi.packettimer){
            phd.packettimeouts++;
            rxi.state = RXI_INIT;
        }
        else
            rxi.packettimer--;
    }
    // Service interrupt holdoff timer
    if(0 == (irq.prescale & 0x1F)){ // 32.768 mSec
        if(irq.timer)
            irq.timer--;
    }

    irq.prescale++;

    // LED activity timer
    if(ledactivitytimer){
        ledactivitytimer--;
        if(ledactivitytimer)
            LED = 0;
        else
            LED = 1;
    }
}

/*
 * Interrupt handler for I2C
 */

static void handle_i2c(void)
{
    if(i2c.busy){
        switch(i2c.priv.state){
            case I2C_SEND_ADDR: /* Start complete */
                SSP1BUF = INI226_ADDR; /* Send Address */
                i2c.priv.state = I2C_SEND_REG;
                break;
                
            case I2C_SEND_REG:
                SSP1BUF = i2c.regptr;
                i2c.priv.state = I2C_READ_WRITE;
                break;

            case I2C_READ_WRITE:
                if(!i2c.rw){ /* Write? */
                    SSP1BUF = (uint8_t) (i2c.reg >> 8); /* High byte out */
                    i2c.priv.state = I2C_WRITE_LOW;
                }
                else{
                    SSP1CON2bits.RSEN = TRUE; /* Read, send restart */
                    i2c.priv.state = I2C_READ_ADDR; /* Send address again */
                }
                break;

            case I2C_WRITE_LOW:
                SSP1BUF = (uint8_t) i2c.reg; /* Low byte out */
                i2c.priv.state = I2C_STOP;
                break;

            case I2C_READ_ADDR:
                SSP1BUF = INI226_ADDR | 1;
                i2c.priv.state = I2C_READ_START;
                break;

            case I2C_READ_START:
                SSP1CON2bits.RCEN = TRUE; /* Set receive enable*/
                i2c.priv.state = I2C_READ_HIGH;
                break;
                
            case I2C_READ_HIGH:
                i2c.reghi = SSP1BUF;
                SSP1CON2bits.ACKDT = FALSE;
                SSP1CON2bits.ACKEN = TRUE;
                i2c.priv.state = I2C_READ_HIGH_ACK;
                break;

            case I2C_READ_HIGH_ACK:
                SSP1CON2bits.RCEN = TRUE; /* Set receive enable */
                i2c.priv.state = I2C_READ_LOW;
                break;

            case I2C_READ_LOW:
                i2c.reglow = SSP1BUF;
                i2c.reg = (i2c.reghi << 8) + i2c.reglow;
                SSP1CON2bits.ACKDT = FALSE;
                SSP1CON2bits.ACKEN = TRUE;
                i2c.priv.state = I2C_STOP;
                break;




            case I2C_STOP:
                SSPCON2bits.PEN = 1; /* Send stop */
                i2c.priv.state = I2C_DONE;
                break;

            case I2C_DONE:
                i2c.busy = FALSE;
                i2c.priv.state = I2C_SEND_ADDR;
                break;

            default:
                i2c.priv.state = I2C_SEND_ADDR;
                break;
        }
    }
}

/*
 * Interrupt service routine
 */

interrupt void isr(void)
{


    /* UART Receive */
    if(PIR1bits.RCIF){
            handle_rda();
            PIR1bits.RCIF = FALSE;
    }

    /* Timer */
    if(INTCONbits.T0IF){
        INTCONbits.T0IF = FALSE;
        handle_timer0();

    }
   
    /* I2C */
    if(PIR1bits.SSP1IF){
        PIR1bits.SSP1IF = FALSE;
        handle_i2c();
   
    }


    /* UART Transmit */
    if(PIE1bits.TXIE && PIR1bits.TXIF){
        PIR1bits.TXIF = FALSE;
        handle_tbe();
    }

}


/*
 * Copy a block from EEPROM to RAM
 */

static void eeprom_to_ram(void *ram, uint8_t eeaddr, uint8_t size)
{
    int i;
    for(i = 0 ; i < size ; i++)
        ((uint8_t *)ram)[i] = eeprom_read(eeaddr + i);


}

/*
 * copy a block from RAM to EEPROM
 */

static void ram_to_eeprom(uint8_t eeaddr, void *ram, uint8_t size)
{
    int i;
    for(i = 0 ; i < size ; i++)
        eeprom_write(eeaddr + i, ((uint8_t *) ram)[i]);
}


static void calc_ina226_cal(void)
{
    uint32_t a107, rs107;

    a107 = 10000000 * eedata.shunt_amps;
    rs107 = (eedata.shunt_mv*(10000000/1000))/eedata.shunt_amps;

    current_lsb = a107 >> 15;
    power_lsb = 25 * current_lsb;
    ina226_cal = (uint16_t) ((512000000)/((current_lsb * rs107 )/1000));
    return;
}

/*
* Calculate 8 bit CRC
*/

static uint8_t calc_crc(uint8_t *p, uint8_t len)
{
    uint8_t theBits, fb;
    uint8_t i, j;

    for(i = 0 ; i < len ; i++)
    {
        CLRWDT();
        theBits = *p++;
        for(j = 0 ; j < 8 ; j++)
        {
            fb = (theBits ^ crcreg) & 1;
            crcreg >>= 1;
            if(fb)
                crcreg ^= POLY;
            theBits >>= 1;
        }
    }
    return crcreg;
}


/*
 * Calculate 16 bit CRC over buffer using polynomial
*/

uint16_t calc_crc16(uint16_t crcin, uint8_t *buf, uint8_t len)
{
    uint8_t i,j;
    static bit dogen;
    union{
        uint16_t word;
        struct{
            uint8_t low;
            uint8_t high;
        };
    } crc;
    
    crc.word = crcin;
    for(i = 0; i < len; i++){
        CLRWDT();
        crc.high ^= buf[i];
        for(j = 0; j < 8; j++){
            dogen = ((crc.high & 0x80) != 0);
            crc.word <<= 1;
            if(dogen){
                crc.word ^= POLY16;
            }
        }
    }
    return crc.word;
}


/*
* Return TRUE if transmitter and holding register are both empty
*/

static bit txisempty()
{
    if(PIR1bits.TXIF){
            if(TXSTAbits.TRMT)
                    return TRUE;
    }
    return FALSE;
}


/*
* Return node ID
*/

static bit do_gnid(uint8_t len, volatile uint8_t *params)
{

    if(4 == len){
            params[0] = (uint8_t) MODULEID;
            params[1] = (uint8_t) (MODULEID >> 8);
            params[2] = (uint8_t) VERSION;
            params[3] = (uint8_t) (VERSION >> 8);
            return NOERR;
    }
    else
            return ERR;
}

/*
* Return comm status
*/

static bit do_gcst(uint8_t len, volatile uint8_t *params)
{

	if(3 == len){
		params[1] = phd.crcerrs;
		params[2] = phd.packettimeouts;
		if(params[0])
			phd.crcerrs = phd.packettimeouts = 0;
		return NOERR;
	}
	else
		return ERR;
}

/*
* Poll for interrupt reason
*/

static bit do_gipl(uint8_t len, volatile uint8_t *params)
{

	params[0] = irq.reason;
	irq.reason = IRQ_REASON_NONE;
	irq.flag = 0;
	return NOERR;
}

/*
 * Return voltage
 */

static bit do_volts(uint8_t len, volatile uint8_t *params)
{
    uint16_t x;
    uint32_t *p = (uint32_t *) (params + 4);

    if((8 == len) && (!params[0])){
        INA226_TRANS_WAIT(INA226_BUS, 1, 0);
        x = INA226_RESULT;
        params[1] = VMAG; // Magnitude
        params[2] = (uint8_t) x;
        params[3] = (uint8_t)(x >> 8);
        *p = VOLTRES;
        return NOERR;
    }
    return ERR;

}

/*
 * Return current
 */

static bit do_current(uint8_t len, volatile uint8_t *params)
{
    uint16_t x;
    uint32_t *p = (uint32_t *) (params + 4);

    if((8 == len) && (!params[0])){
        INA226_TRANS_WAIT(INA226_CURRENT, 1, 0);
        x = INA226_RESULT;
        params[1] = CMAG; // Magnitude
        params[2] = (uint8_t) x;
        params[3] = (uint8_t)(x >> 8);
        *p = current_lsb;
        return NOERR;

    }
    return ERR;

}


/*
 * Return power
 */

static bit do_power(uint8_t len, volatile uint8_t *params)
{
    uint16_t x;
    uint32_t *p = (uint32_t *) (params + 4);

    if((8 == len) && (!params[0])){
        INA226_TRANS_WAIT(INA226_POWER, 1, 0);
        x = INA226_RESULT;
        params[1] = PMAG; // Magnitude
        params[2] = (uint8_t) x;
        params[3] = (uint8_t)(x >> 8);
        *p = power_lsb;
        return NOERR;

    }
    return ERR;

}

/*
 * Allow user to read and write the shunt config
 */

static bit do_shunt_config(uint8_t len, volatile uint8_t *params)
{
    uint16_t *words = (uint16_t *) params;

    if(4 == len){
        if( 0 == params[0]){ /* Read config? */
            words[1] = eedata.shunt_amps;
            params[1] = eedata.shunt_mv;
            return NOERR;
        }
        else if(1 == params[0]){ /* Write config? */
            // sanity check values
            if((words[1] <= 200) && (words[1] > 0) &&
               (params[1] <= 80 && (params[1] > 0))){
                eedata.shunt_amps = words[1];
                eedata.shunt_mv = params[1];
                calc_ina226_cal(); // calculate new cal value
                INA226_TRANS_WAIT(INA226_CAL, 0, ina226_cal); // update cal
                ram_to_eeprom(0, &eedata, sizeof(eedata)); // eeprom write
                return NOERR;
            }
        }
        else if (2 == params[0]){ /* Return cal for diagnostic purposes */
            params[1] = 0;
            words[0] = ina226_cal; // send cal back with return data
            return NOERR;
        }
    }
    return ERR;
}

/*
 * Set or read output bits
 */


static bit do_gout(uint8_t len, volatile uint8_t *params)
{
	uint8_t res;

	if(len == 3){
		if(params[0] > 1) // Channel
			return ERR;
		if(params[2] != 0) // Result
			return ERR;
		switch(params[1]){ // Command
			case 0:
                                switch(params[0]){
                                    case 0:
                                        OD1 = OFF;
                                        break;
                                    case 1:
                                        OD2 = OFF;
                                        break;
                                }
				break;

			case 1:
                                switch(params[0]){
                                    case 0:
                                        OD1 = ON;
                                        break;
                                    case 1:
                                        OD2 = ON;
                                        break;
                                }
				break;
			case 2:
				switch(params[0]){
					case 0:
						res = OD1;
						break;
					case 1:
						res = OD2;
						break;
				}
				params[2] = res;
				break;
			default:
				return ERR;
		}
		return NOERR;
	}
	else
		return ERR;

}

/*
* Raise interrupt request
*/

static void raise_irq(uint8_t reason)
{
	irq.holdoff = myaddress & 0x1F;
	irq.reason = reason;
	irq.timer = irq.holdoff;
	irq.flag = TRUE;

}



#ifdef BOOTAPP
/*
* Enter boot loader
*/
static bool do_enterbootloader(uint8_t len, uint8_t *params)
{
	int i;
	if((2 == len) && (0x55 == params[0]) && (0xAA == params[1])){
		enterbootloader=TRUE;
		return PASS;
	}
	return FAIL;
}

#endif

/*
* State machine to service packets
*/

void service_packets(void)
{
	uint16_t crc16;
	uint8_t i,crc,len;

	// Packet Service
	switch(phd.state){
		case PHD_START:
			if(rxi.pready){
				phd.rxerr = 0;
                                /* Handy u8 * reference */
				phd.pktb = (uint8_t *) &pkt; 
				phd.state = PHD_PKT_READY;
			}
			else if((irq.flag) && (0 == irq.timer) &&
                        (!ADDRPROGMODE)){
                            /* This block sends an interrupt request */
                            uint8_t holdoff = irq.holdoff;
                            /* Provide a pseudo random delay time. */
                            irq.holdoff = calc_crc(&holdoff, 1); 
                            irq.timer = irq.holdoff & 0x1F;
                            crcreg = 0;
                            phd.crcword = TRUE;
                            pkt.hcb = HDCIRQ16;
                            pkt.addr = myaddress;
                            phd.pktb = (uint8_t *) &pkt;
                            txi.blen = PKTIRQLEN;
                            phd.state = PHD_TX_START;
			}
			break;

		case PHD_PKT_READY:
                        /* If wrong header */
			if((pkt.hcb != HDC) && (pkt.hcb != HDC16)){ 
                            phd.state = PHD_FIN;
                            break;
			}

			if(rxi.index >= MAXPACKET){ // If too long
                            phd.state = PHD_FIN;
                            break;
			}

			if(HDC == pkt.hcb){ // 8 bit CRC
                            phd.crcword = FALSE;
                            crcreg = 0;
                            crc = calc_crc(phd.pktb, rxi.index - 1);
                            if(phd.pktb[rxi.index-1] != crc){ // If CRC error
				phd.crcerrs++;
				phd.state = PHD_FIN;
				break;
                            }
			}
			else{ // 16 bit CRC
                            uint16_t packetcrc = phd.pktb[rxi.index - 2] +
                            (((uint16_t) phd.pktb[rxi.index - 1]) << 8);
                            phd.crcword = TRUE;
                            crc16 = calc_crc16(0, phd.pktb, rxi.index - 2);

                            if(packetcrc != crc16){ // If CRC error
                                phd.crcerrs++;
				phd.state = PHD_FIN;
				break;
                            }
			}
			phd.state = PHD_PKT_DECODE;
			break;

		case	PHD_PKT_DECODE:

			if(!ADDRPROGMODE){
                            /* If not our address or broadcast address */
                            if((pkt.addr != myaddress) && (pkt.addr != 0xFF)){ 
				phd.state = PHD_FIN;
				break; // Not for us
                            }
			}
			else{
                            if(PADD == pkt.cmd){ // Program address
				myaddress = pkt.addr;
				eeprom_write(EEADDR, myaddress);
                            }
                            else{
				phd.state = PHD_FIN;
				return;
                            }
			}

			if(!ADDRPROGMODE){
                            /* Compute parameter length */
                            len = rxi.index - ((phd.crcword) ?
                            (PKTCTRL + 2) : (PKTCTRL + 1)); 
                            #ifdef BOOTAPP
                            enterbootloader = FALSE;
                            #endif

                            // Decode command

                            if(pkt.addr == myaddress){
                                switch(pkt.cmd){
                                    case NOOP: // No Operation
					break;

                                    case GNID: // Node ID
                                        phd.rxerr = do_gnid(len, pkt.params);
					break;

                                    case GCST: // Comm Status
                                        phd.rxerr = do_gcst(len, pkt.params);
					break;

                                    case GIPL: // Poll Interrupt reason
                                        phd.rxerr =  do_gipl(len, pkt.params);
                                        break;

                                    case GOUT:
                                        phd.rxerr = do_gout(len, pkt.params);
                                        break;

                                    case GVLT: // Return voltage
                                        phd.rxerr = do_volts(len, pkt.params);
                                        break;

                                    case GCUR: // Return current
                                        phd.rxerr = do_current(len, pkt.params);
                                        break;

                                    case GPWR: // Return power
                                        phd.rxerr = do_power(len, pkt.params);
                                        break;

                                    case GSCF:
                                        phd.rxerr = do_shunt_config(len, pkt.params);
                                        break;

                                    #ifdef BOOTAPP
                                    case GEBL:	// Enter boot loader
                                        phd.rxerr = do_enterbootloader(len,
                                        pkt.params);
                                        break;
                                    #endif

                                    default:
                                        phd.rxerr = TRUE;
				}
                            }
                           else{ // Must be a broadcast packet
                                switch(pkt.cmd){
                                   case BCP_ENUM: // Enumerate
                                        raise_irq(IRQ_REASON_NONE);
                                        break;

                                        default:
                                            break;
                                }
                                phd.state = PHD_FIN;
                                break; // Broadcast packets are not Ack'ed
                            }
			}
			phd.state = PHD_PKT_RESP;
			break;

		case PHD_PKT_RESP:

                    // Send Response;
                    if(phd.rxerr)
                        pkt.hcb = (phd.crcword) ? HDC_NAK16 : HDC_NAK;
                    else
                        pkt.hcb = (phd.crcword) ? HDC_ACK16 : HDC_ACK;

                    phd.state = PHD_TX_START;
                    txi.blen = rxi.index;
                    break;

		case PHD_TX_START:

                    TXENA = TRUE;	// Enable TX

                    //calculate return CRC

                    if(!phd.crcword){ // 8 bit response CRC
                        crcreg = 0;
                        crc = calc_crc(phd.pktb,txi.blen - 1);
			phd.pktb[txi.blen - 1] = crc;
                    }
                    else{ // 16 bit response CRC
                        uint16_t pktcrc = calc_crc16(0, phd.pktb, txi.blen - 2);
                        phd.pktb[txi.blen - 2] = (uint8_t) pktcrc;
                        phd.pktb[txi.blen - 1] = pktcrc >> 8;
                    }

                    // Send the response
                    phd.state = PHD_WAIT_TX;
                    txi.txbusy = TRUE;
                    PIE1bits.TXIE = TRUE; // Enable TX interrupt
                    break;

		case PHD_WAIT_TX:
                    if(!txi.txbusy){
                        phd.state = PHD_WAIT_EMPTY;
                    }
                    break;

		case PHD_WAIT_EMPTY:
                    if(txisempty()){
                        TXENA = FALSE;	// Disable TX
                        phd.state = PHD_FIN;
                        ledactivitytimer = 0xFF;
                    }
                    break;

		case PHD_FIN:
                    #ifdef BOOTAPP
                    if(((HDC_ACK == pkt.hcb) || (HDC_ACK16 == pkt.hcb)) &&
                    (GEBL == pkt.cmd) && enterbootloader){
                        for(i = 0 ; i < 3; i++){
                            write_eeprom(EEBOOTSIG, 0x55);
                            delay_ms(10);
                            if(0x55 == read_eeprom(EEBOOTSIG))
                                break;
			}
			disable_interrupts(GLOBAL);
			delay_ms(10);
			reset_cpu();
			}
                    #endif
                    rxi.pready = FALSE;
                    phd.state = PHD_START;
                    break;

		default:
                    rxi.pready = FALSE;
                    phd.state = PHD_START;
		break;

	} // end switch

}



int main(void) {
    uint8_t i;

    /*
     * Init
     */
    OSCCON = 0x70; // Select 8MHz source for PLL

    /* Pin selects */
    APFCON0 = 0x00;
    APFCON1 = 0x00;

    /* Port A */
    ANSELA = 0x00;
    TRISA = 0x07;
    WPUA = 0x07;
    PORTA = 0x00;

    /* Port C */
    ANSELC = 0x00;
    TRISC = 0x23;
    WPUC = 0x00;
    PORTC = 0x00;
    
    /* UART */
    SPBRGL = SET_BAUD(9600);
    RCSTA = 0x90;
    TXSTA = 0x20;

    /* I2C */
    SSP1CON1 = 0x8;
    SSP1CON3 = 0x00;
    SSPADD = 0x4F;
    SSPSTAT = 0x80;
    PIR1bits.SSP1IF = FALSE;
    SSP1CON1bits.SSPEN = TRUE;


    /* Timer 0 */
    OPTION_REG = 0x04; /* 976.5625 Hz 1.024 mSec */

    myaddress = eeprom_read(EEADDR);
    if(0xFF == myaddress) // If EEPROM erased
        myaddress = 0x1F; // Use test address 0x1F
    /* Fetch config */
    eeprom_to_ram(&eedata, EECONFIGSTART, sizeof(eedata_t));
    if(eedata.sig != EESIG){
        for(i = 2; i < sizeof(eedata_t); i++)
            eedata.bytes[i] = 0;
        eedata.sig = EESIG;
        eedata.shunt_amps = DEF_SHUNT_AMPS;
        eedata.shunt_mv = DEF_SHUNT_MV;
        ram_to_eeprom(EECONFIGSTART, &eedata, sizeof(eedata_t));
    }

    /* Interrupt enables */
    PIE1bits.SSP1IE = TRUE;
    PIE1bits.RCIE = TRUE;
    INTCON = 0xE0;

    /* Calculate INA226 calibration constant */
    calc_ina226_cal();
    
    /* Set up INA226 */
    INA226_TRANS_WAIT(INA226_CONFIG, 0, INA226_INIT_CONFIG);
    INA226_TRANS_WAIT(INA226_CAL, 0, ina226_cal);
 




    // Set at boot interrupt
    raise_irq(IRQ_REASON_ATBOOT);

    ledactivitytimer = 0x3F; // Short flash to indicate start up
   
    /*
     * Foreground loop
     */

    while (1) {
        CLRWDT();
        service_packets();

    }
    return 0;
}
