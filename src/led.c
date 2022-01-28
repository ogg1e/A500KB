/*
 ********************************************************************************
 * led.c                                                                        *
 *                                                                              *
 * Author: Henryk Richter <bax@comlab.uni-rostock.de>                           *
 *                                                                              *
 * Purpose: LED controller interface                                            *
 *                                                                              *
 *                                                                              *
 *                                                                              *
 *                                                                              *
 *                                                                              *
 ********************************************************************************
*/
/*
  Gamma table
  track LED source changes -> send updated colors

  done: LED source map for LED 0-6
  LED color table idle/active
  LED mode regular/special (knight rider, rainbow, cycle)
*/

#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h> 
//#include <avr/io.h>
#include <util/delay.h> /* might be <avr/delay.h>, depending on toolchain */
//#include "avr/delay.h"
#include "baxtypes.h"
#include "twi.h"
#include "kbdefs.h"
#include "led.h"
#include "gammatab.h"

#define DEBUGONLY
#ifdef DEBUG
#include "uart.h"
#define DBGOUT(a) uart1_putc(a);
#define DEBUG_ON()
#else
#define DBGOUT(a)
#define DEBUG_ON()
#endif

#ifndef NULL
#define NULL (0)
#endif

#if 0
uint8_t eeprom_read_byte (const uint8_t *__p);
void eeprom_read_block (void *__dst, const void *__src, size_t __n);

void eeprom_write_byte (uint8_t *__p, uint8_t __value);

void eeprom_update_byte (uint8_t *__p, uint8_t __value);
void eeprom_update_word (uint16_t *__p, uint16_t __value);
void eeprom_update_block (const void *__src, void *__dst, size_t __n);

Suppose we want to write a 55 value to address 64 in EEPROM, then we can write it as,
uint8_t ByteOfData = 0 x55 ;
eeprom_update_byte (( uint8_t *) 64, ByteOfData );
#endif

/* set/clear Bit in register or port */
#define BitSet( _port_, _bit_ ) _port_ |=   (1 << (_bit_) )
#define BitClr( _port_, _bit_ ) _port_ &= (~(1 << (_bit_) ) )
/* copy bit from source to destination (2 clocks via T Bit) */
#define BitCpy( _out_, _obit_, _in_, _ibit_ ) \
	asm volatile ( "bst %2,%3" "\n\t" \
	               "bld %0,%1" : "+r" (_out_) : "I" (_obit_) , "r" (_in_) , "I" (_ibit_) );

/* put number on UART */
extern void uart_puthexuchar(unsigned char a);
extern void uart_puthexuint(uint16_t a);

/* LED controller address (twi.c needs address >>1) */
#define I2CADDRESS (0x68>>1)

/* 
  init string: 
   length, sequence
  terminated with length 0
*/
#define MAXLEDINITSEQ 8
const unsigned char ledinitlist[] PROGMEM = {
 2, 0x00, 0x07, /* [control register 0],[16 bit (0x6), normal op (0x1), 16 Bit (%000<<4)] */
 2, 0x6E, 0xFF, /* [Global Current Control],[enable full current (0x1-0xFF)]              */
 2, 0x70, 0xBF, /* [Phase Delay and Clock Phase],[PDE=1,PSn=1] */
 /* end of list */
 0
};

#if 1
/* LED Sources as flags used for state and change tracking */
#define LEDB_SRC_POWER  0
#define LEDB_SRC_FLOPPY 1
#define LEDB_SRC_IN3    2
#define LEDB_SRC_IN4    3
#define LEDB_SRC_CAPS   4
#define LEDF_SRC_POWER  (1<<LEDB_SRC_POWER)
#define LEDF_SRC_FLOPPY (1<<LEDB_SRC_FLOPPY)
#define LEDF_SRC_IN3    (1<<LEDB_SRC_IN3)
#define LEDF_SRC_IN4    (1<<LEDB_SRC_IN4)
#define LEDF_SRC_CAPS   (1<<LEDB_SRC_CAPS)
#define LEDF_ALL ( (LEDF_SRC_POWER)|(LEDF_SRC_FLOPPY)|(LEDF_SRC_IN3)|(LEDF_SRC_IN4)|(LEDF_SRC_CAPS) )

/* number of LEDs */
#define N_LED         7

/* end of command list */
#define TCMD_END 0xff

/* current state */
#define LED_IDLE      0 /* idle             */
#define LED_ACTIVE    1 /* primary   active */
#define LED_SECONDARY 2 /* secondary active */
#define LED_STATES    3 

extern unsigned char caps_on; /* CAPSLOCK state 0=off,1=on */

unsigned char led_currentstate; /* current input source state    */
unsigned char src_active;       /* source state as sent to LEDs  */

unsigned char LED_SRCMAP[N_LED]; /* flags applying to this LED      */
unsigned char LED_RGB[N_LED][LED_STATES][3]; /* RGB config for LEDs */
unsigned char LED_MODES[N_LED];  /* static,cycle, rainbow, knight rider etc. */

/* 
  TWI command list: 
   1 Byte LED, 1 Byte STATE
  Terminated by 0xFF,0xFF
*/
unsigned char twicmds[(N_LED+1)*2];
uint8_t initseq[MAXLEDINITSEQ];

#endif

void adc_start( unsigned  char channel )
{
  ADMUX  = (ADMUX&0xE0)|(channel & 0x7); /* select ADC channel 0 (channels 0-7 in lower 3 bits) */
  ADCSRA |= (1<<ADSC); /* start ADC (single conversion) */
}

uint16_t adc_get( void )
{ 
  uint8_t  AinLow;
  uint16_t Ain;

  AinLow = (int)ADCL;         /* lower 8 bit */
  Ain = ((uint16_t)ADCH)<<8;  /* upper two bit, shifted */
  Ain = Ain|AinLow;

  return Ain;
}

unsigned char update_bit( unsigned char old_state, unsigned char decision, unsigned char flag )
{
 unsigned char ret;

 ret = (decision) ? old_state|flag : old_state&(~flag);

 return ret;
}

/* scan inputs, return current state of all inputs, return quickly */
/*
   some inputs may be analog and need to be muxed,
   hence the returned state is not instantaneus. 
   
   This function will initialize ADC and update the input states
   once an ADC cycle is completed. 

*/
unsigned char adc_cycle;
unsigned char led_getinputstate()
{
 uint16_t ad;

	led_currentstate = (caps_on) ? led_currentstate|LEDF_SRC_CAPS : led_currentstate&(~LEDF_SRC_CAPS);

	/* cycle through ADCs */
	switch( adc_cycle )
	{
		case 0:
			adc_start(0); /* channel0: POWER LED */
			adc_cycle++;
			break;
		case 1: /* Power LED: >4.15V = ON (5*850/1024) */
			if( (ADCSRA&(1<<ADIF))==0 ) /* wait for ADC */
				break;
			ad = adc_get( );
			led_currentstate = update_bit( led_currentstate, ad>850, LEDF_SRC_POWER );
			adc_cycle++;
				break;
			//led_currentstate = (ad > 850) ?  led_currentstate|LEDF_SRC_POWER : led_currentstate&(~LEDF_SRC_POWER);

		case 2:
			adc_start(1); /* channel1: FLOPPY LED */
			adc_cycle++;
			break;
		case 3:	/* FLOPPY LED: >2.5V = on, else off */
			if( (ADCSRA&(1<<ADIF))==0 ) /* wait for ADC */
				break;
			ad = adc_get();
			led_currentstate = update_bit( led_currentstate, ad>512, LEDF_SRC_FLOPPY );
			adc_cycle++;
			break;
			//led_currentstate = (ad > 512) ? led_currentstate|LEDF_SRC_FLOPPY : led_currentstate&(~LEDF_SRC_FLOPPY);

		case 4: /* IN3, IN4: digital for now, low active */
			led_currentstate = update_bit( led_currentstate, 0==(IN3LED_PIN&(1<<IN3LED_BIT)), LEDF_SRC_IN3 );
			led_currentstate = update_bit( led_currentstate, 0==(IN4LED_PIN&(1<<IN4LED_BIT)), LEDF_SRC_IN4 );
			adc_cycle++;
			break;

		default:
			adc_cycle = 0;
			break;
	}

	return led_currentstate;
}

unsigned char twi_ledupdate_pos;
unsigned char led_sending;
void twi_ledupdate_callback( uint8_t address, uint8_t *data )
{
	unsigned char i,t;

	/* a little delay between consecutive TWI writes */
	_delay_us(4);

	i=twicmds[twi_ledupdate_pos]; /* get target LED */
	if( i == TCMD_END ) /* end of list: commit changes */
	{
		initseq[0] = 0x49;
		initseq[1] = 0x00;
		twi_write(I2CADDRESS, initseq, 2, (0) ); /* No more callback */
		led_sending = 0;
		return;
	}
	t = twicmds[twi_ledupdate_pos+1]; /* get state index */

	initseq[0] = 0x01+i*3*2; /* LED index to hardware register */
#if 1
	initseq[1] = pgm_read_byte(&gamma24_tableLH[LED_RGB[i][t][0]][GAMMATAB_L]); /* red L */
	initseq[2] = pgm_read_byte(&gamma24_tableLH[LED_RGB[i][t][0]][GAMMATAB_H]); /* red H */
	initseq[3] = pgm_read_byte(&gamma24_tableLH[LED_RGB[i][t][1]][GAMMATAB_L]); /* grn L */
	initseq[4] = pgm_read_byte(&gamma24_tableLH[LED_RGB[i][t][1]][GAMMATAB_H]); /* grn H */
	initseq[5] = pgm_read_byte(&gamma24_tableLH[LED_RGB[i][t][2]][GAMMATAB_L]); /* blu L */
	initseq[6] = pgm_read_byte(&gamma24_tableLH[LED_RGB[i][t][2]][GAMMATAB_H]); /* blu H */
#else
	/* TODO: Gamma table */
	initseq[1] = (LED_RGB[i][t][0]<<4);  /* red L */
	initseq[2] = (LED_RGB[i][t][0]>>4);  /* red H */
	initseq[3] = (LED_RGB[i][t][1]<<4);  /* grn L */
	initseq[4] = (LED_RGB[i][t][1]>>4);  /* grn H */
	initseq[5] = (LED_RGB[i][t][2]<<4);  /* blu L */
	initseq[6] = (LED_RGB[i][t][2]>>4);  /* blu H */
#endif
	twi_ledupdate_pos+=2;

	twi_write(I2CADDRESS, initseq, 7, twi_ledupdate_callback );
}

unsigned char led_putcommands( unsigned char *recvcmd, unsigned char nrecv )
{
	unsigned char index,st,r,g,b;
	char confget = -1;
	unsigned char *sendbuf = recvcmd; /* just re-use the command buffer */

	while( nrecv )
	{
		index = *recvcmd & LEDINDEX_MASK;

		nrecv--;
		switch( *recvcmd++ & LEDCMD_MASK )
		{
			case LEDCMD_GETCONFIG:
				if( !nrecv )
					break;
				nrecv--;
				recvcmd++;	/* skip argument byte (unused for now) */
				if( index >= N_LED )
					break;
				confget = index; /* trigger: this LED's config is needed */
				/* FIXME: only one config per call, for now -> also: size limitation of command and ring buffers */
				break;
			case LEDCMD_SOURCE:
				if( !nrecv )
					break;
				nrecv--;
				r = *recvcmd++;
				if( index >= N_LED )
					break;
				LED_SRCMAP[index] = r;
				break;

			case LEDCMD_COLOR:
				if( nrecv < 4 )
				{
					nrecv = 0; /* insufficient data, stop loop */
					break;
				}
				nrecv -= 4;
				st = *recvcmd++;
				r  = *recvcmd++;
				g  = *recvcmd++;
				b  = *recvcmd++;

				if( (st < LED_STATES) && (index < N_LED) )
				{
					LED_RGB[index][st][0] = r;
					LED_RGB[index][st][1] = g;
					LED_RGB[index][st][2] = b;
				}
				break;

			default: /* unhandled command: stop loop */
				nrecv = 0;
				break;
		}
	}

	/* if we got the command to upload our config, just dump it into the
	   command receive buffer and return the number of bytes

           That data is inserted into the outgoing stream before ACK/NACK
	*/
	if( confget >= 0 ) /* def: -1 */
	{
		*sendbuf++ = LED_SRCMAP[(unsigned char)confget];
		for( st = 0 ; st < LED_STATES ; st++ )
		{
			*sendbuf++ = LED_RGB[(unsigned char)confget][st][0];
			*sendbuf++ = LED_RGB[(unsigned char)confget][st][1];
			*sendbuf++ = LED_RGB[(unsigned char)confget][st][2];
		}
		return (LED_STATES*3)+1;
	}

	return 0;
}

/* apply input state to LEDs */
unsigned char led_updatecontroller( unsigned char state )
{
	unsigned char chg,*tcmd,i,t;

	chg = src_active^state; /* changed inputs */
	if( !chg )
		return state;
	if( twi_isbusy() )
		return state;
	if( led_sending )
		return state;

	if( state & LED_FORCE_UPDATE )
		chg = LEDF_ALL; /* force all changed */
	state &= LEDF_ALL;
	/* traverse LEDs and issue commands into TWI wait queue
	   concept: write LED index and state into command buffer,
	            which is translated into TWI sequences in it's
		    write callback (plus "confirm" command)
	*/
	tcmd = twicmds; /* generate new command list */

	for( i=0 ; i < N_LED ; i++ )
	{
		/* does the change in sources apply to this LED? */
		if( !( LED_SRCMAP[i] & chg ))
			continue; /* no, next */
		*tcmd++ = i; /* this LED needs new RGB */
		t = LED_SRCMAP[i] & state; /* on/off state vs. assigned bit(s) */
		/* TODO: more states: secondary RGB */
		if( t )
			*tcmd++ = 1;
		else	*tcmd++ = 0;
	}

	/* TODO: respect LED_MODES */
	led_sending = 1;
	*tcmd++ = TCMD_END;
	*tcmd++ = TCMD_END;

	twi_ledupdate_pos = 0;    /* start of list */
	twi_ledupdate_callback(0,(0)); /* the callback will start to send the list */

	src_active = state; /* we did everything, save this state */

	return state;
}


/* TODO: better TWI receive function (but not needed in this code) */
unsigned char twirec;
void twi_callback(uint8_t adr, uint8_t *data)
{
	twirec=*data;
}


void led_defaults()
{
	uint8_t i;

	/* 0,1,2 are the floppy LED (left,mid,right) */
	LED_SRCMAP[0] = LEDF_SRC_FLOPPY;
	LED_SRCMAP[1] = LEDF_SRC_FLOPPY;
	LED_SRCMAP[2] = LEDF_SRC_IN3;

	LED_SRCMAP[3] = LEDF_SRC_POWER;
	LED_SRCMAP[4] = LEDF_SRC_POWER;
	LED_SRCMAP[5] = LEDF_SRC_POWER;

	LED_SRCMAP[6] = LEDF_SRC_CAPS;

	/* RGB defaults */
	for( i=0 ; i < 2 ; i++ )
	{ /* floppy */
		LED_RGB[i][LED_IDLE][0] = 0x00;
		LED_RGB[i][LED_IDLE][1] = 0x00;
		LED_RGB[i][LED_IDLE][2] = 0x00;
		LED_RGB[i][LED_ACTIVE][0] = 0xFF; /* orange */
		LED_RGB[i][LED_ACTIVE][1] = 0x90;
		LED_RGB[i][LED_ACTIVE][2] = 0x00;
	}
	/* IN3 */
	i=2;
	LED_RGB[i][LED_IDLE][0] = 0x00;
	LED_RGB[i][LED_IDLE][1] = 0x00;
	LED_RGB[i][LED_IDLE][2] = 0x00;
	LED_RGB[i][LED_ACTIVE][0] = 0x00; /* cyan */
	LED_RGB[i][LED_ACTIVE][1] = 0xEA;
	LED_RGB[i][LED_ACTIVE][2] = 0xFF;

	/* Power */
	for( i=3 ; i < 6 ; i++ )
	{ 
		LED_RGB[i][LED_IDLE][0] = 0x20;
		LED_RGB[i][LED_IDLE][1] = 0x01+((i-3)<<2);
		LED_RGB[i][LED_IDLE][2] = 0x04+((i-3)<<1);
		LED_RGB[i][LED_ACTIVE][0] = 0xFF; /* red-ish */
		LED_RGB[i][LED_ACTIVE][1] = 0x03+((i-3)<<5);
		LED_RGB[i][LED_ACTIVE][2] = 0x46+((i-3)<<4);
	}

	/* Caps */
	i=6;
	LED_RGB[i][LED_IDLE][0] = 0x01;
	LED_RGB[i][LED_IDLE][1] = 0x32;
	LED_RGB[i][LED_IDLE][2] = 0x26;
	LED_RGB[i][LED_ACTIVE][0] = 0xA1; /* violet */
	LED_RGB[i][LED_ACTIVE][1] = 0x00;
	LED_RGB[i][LED_ACTIVE][2] = 0xC9;

}

void led_init()
{
  unsigned char i,j;

  led_currentstate = 0; /* all off/idle */
  adc_cycle = 0;
  src_active = 0;       /* source state as sent to LEDs  */

  /* LED sources */
  DRVLED_DDR  &= ~(1<<DRVLED_BIT);
  DRVLED_PORT &= ~(1<<DRVLED_BIT); /* no pull-up, used as analog input */
//  DRVLED_PORT |=  (1<<DRVLED_BIT); /* in, with pull-up */

  IN3LED_DDR  &= ~(1<<IN3LED_BIT);
  IN3LED_PORT &= ~(1<<IN3LED_BIT); // |=  (1<<IN3LED_BIT);
  IN4LED_DDR  &= ~(1<<IN4LED_BIT);
  IN4LED_PORT &= ~(1<<IN4LED_BIT); // |=  (1<<IN4LED_BIT);

  PLED_DDR    &=  ~(1<<PLED_BIT);
  PLED_PORT   &=  ~(1<<PLED_BIT); /* we use this as analog input */

  ADCSRA = 0x87;                  /* Enable ADC, fr/128  */
  ADMUX  = 0x40;                  /* Vref: Avcc, ADC channel: 0 (PortF on AT90USB1287 */

#if 0
  /* ADC test */
  ADMUX  = (ADMUX&0xE0)|(0x00 & 0x7); /* select ADC channel 0 (channels 0-7 in lower 3 bits) */
  ADCSRA |= (1<<ADSC); /* start ADC (single conversion) */

  while((ADCSRA&(1<<ADIF))==0); /* wait for ADC */
  _delay_us(10);
  { uint8_t  AinLow;
    uint16_t Ain;
    AinLow = (int)ADCL;		/* lower 8 bit */
    Ain = ((uint16_t)ADCH)<<8;	/* upper two bit, shifted */
    Ain = Ain|AinLow;

    uart_puthexuint(Ain);
    DBGOUT(13);
    DBGOUT(10);
  }
#endif


  /* set up I2C communication with LED controller */
  twi_init();
  DDRD |= (1<<5); /* Pin5 = SDB = Output */
  PORTD|= (1<<5); /* Pin5 = SDB = High   -> LED controller on */

  /* configure ports 1-21, RGB ordered */
  {
    uint8_t  *is;
    uint16_t idx;

	/* from table */
	idx=0;
	while( (i=pgm_read_byte(&ledinitlist[idx++])) != 0 )
	{
		is=initseq;
		j=i;
		while(j--)
		{
			*is++ = pgm_read_byte(&ledinitlist[idx++]);
		}
		twi_write(I2CADDRESS, initseq, i, NULL );
	};

	/* RGB balance (white balance) */
  	for( i=0 ; i < 21 ; i+=3 ) /* 21 outputs for 7 LEDs used */
  	{
		initseq[0] = 0x4A + i; /* target register */ 
		initseq[1] = 0xFF;     /* R */ 
		initseq[2] = 0xFF;     /* G */ 
		initseq[3] = 0x5F;     /* B */
		twi_write(I2CADDRESS, initseq, 4, NULL );
	}

	/* set RGB defaults and input port assignments */
	led_defaults();

	/* write some RGB values */
#if 0
	i=0;
	initseq[0] = 0x01+i*3*2;
	initseq[1] = 0xFF; /* red L */
	initseq[2] = 0x1F; /* red H */
	initseq[3] = 0xFF; /* green L */
	initseq[4] = 0x1F; /* green H */
	initseq[5] = 0xFF; /* blue L */
	initseq[6] = 0x1F; /* blue H */
	twi_write(I2CADDRESS, initseq, 7,NULL );

	i=3;
	initseq[0] = 0x01+i*3*2;
	initseq[1] = 0xFF; /* red L */
	initseq[2] = 0x00; /* red H */
	initseq[3] = 0x00; /* green L */
	initseq[4] = 0x00; /* green H */
	initseq[5] = 0x00; /* blue L */
	initseq[6] = 0x00; /* blue H */
	twi_write(I2CADDRESS, initseq, 7,NULL );
	i=4;
	initseq[0] = 0x01+i*3*2;
	initseq[1] = 0x00; /* red L */
	initseq[2] = 0x00; /* red H */
	initseq[3] = 0xff; /* green L */
	initseq[4] = 0x00; /* green H */
	initseq[5] = 0x00; /* blue L */
	initseq[6] = 0x00; /* blue H */
	twi_write(I2CADDRESS, initseq, 7,NULL );
	i=5;
	initseq[0] = 0x01+i*3*2;
	initseq[1] = 0x00; /* red L */
	initseq[2] = 0x00; /* red H */
	initseq[3] = 0x00; /* green L */
	initseq[4] = 0x00; /* green H */
	initseq[5] = 0xff; /* blue L */
	initseq[6] = 0x00; /* blue H */
	twi_write(I2CADDRESS, initseq, 7,NULL );
	i=6;
	initseq[0] = 0x01+i*3*2;
	initseq[1] = 0xff; /* red L */
	initseq[2] = 0x00; /* red H */
	initseq[3] = 0xff; /* green L */
	initseq[4] = 0x00; /* green H */
	initseq[5] = 0xff; /* blue L */
	initseq[6] = 0x00; /* blue H */
	twi_write(I2CADDRESS, initseq, 7,NULL );
#else
#if 0
	/* debug: light some LEDs */
	j=0xF0;
	for( i=0 ; i < 21 ; i++ ) 
	{
		initseq[0] = 0x01+i*2; /* PWM Low register (0x2 would be high) */
		initseq[1] = j;
		j ^= 0xFF;
		twi_write(I2CADDRESS, initseq, 2, NULL );
	}
#endif
#endif
#if 0
	/* confirm changes */
	initseq[0] = 0x49;
	initseq[1] = 0x00;
	twi_write(I2CADDRESS, initseq, 2, NULL );
#endif
#if 0
	/* DEBUG: READ settings */
	initseq[0] = 0x00;
	twi_write(I2CADDRESS, initseq, 1, NULL );
	twi_read(I2CADDRESS,1,twi_callback);
	uart_puthexuchar(twirec);
	DBGOUT(13);DBGOUT(10);

	initseq[0] = 0x02;
	twi_write(I2CADDRESS, initseq, 1, NULL );
	twi_read(I2CADDRESS,1,twi_callback);
	uart_puthexuchar(twirec);
	DBGOUT(13);DBGOUT(10);
#endif


  }


}



