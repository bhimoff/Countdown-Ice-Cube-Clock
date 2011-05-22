/***************************************************************************
 Ice Tube Clock firmware August 13, 2009
 (c) 2009 Limor Fried / Adafruit Industries
 Modifications by Len Popp
 Original auto-dimmer mod by Dave Parker
 Button interrupt fix by caitsith2
 Daylight Saving Time code
    Rule selection code by caitsith2
    Canada/US rules by caitsith2
    Germany rules by bastard
 Testmode feature by caitsith2
 Countdown by Brandon Himoff

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
****************************************************************************/


#include <avr/io.h>      
#include <string.h>
#include <avr/interrupt.h>   // Interrupts and timers
#include <util/delay.h>      // Blocking delay functions
#include <avr/pgmspace.h>    // So we can store the 'font table' in ROM
#include <avr/eeprom.h>      // Date/time/pref backup in permanent EEPROM
#include <avr/wdt.h>     // Watchdog timer to repair lockups

#include "iv.h"
#include "util.h"
#include "fonttable.h"

uint8_t region = REGION_US;

// These variables store the current time.
volatile uint8_t time_s, time_m, time_h;
// ... and current date
volatile uint8_t date_m, date_d, date_y;
// ... and the countdown
volatile int16_t cd_d;
volatile int8_t cd_h, cd_m, cd_s;

// hold end of countdown
volatile uint8_t end_year, end_month, end_day, end_hour, end_min;


// how loud is the speaker supposed to be?
volatile uint8_t volume;

// brightness set by user - 0 means the auto dimmer is turned on 
volatile uint8_t brightness_level;
#ifdef FEATURE_TESTMODE
  #ifdef FEATURE_AUTODIM
      volatile uint16_t dimmer_level;
  #endif
#endif

// whether the alarm is on, going off, and alarm time
volatile uint8_t alarm_on, alarming, alarm_h, alarm_m;

// what is being displayed on the screen? (eg time, date, menu...)
volatile uint8_t displaymode;

// are we in low power sleep mode?
volatile uint8_t sleepmode = 0;

volatile uint8_t timeunknown = 0;        // MEME
volatile uint8_t restored = 0;

#ifdef FEATURE_DST
volatile uint8_t dst_on;
volatile uint8_t dst_set = 0;
#endif

// Our display buffer, which is updated to show the time/date/etc
// and is multiplexed onto the tube
uint8_t display[DISPLAYSIZE]; // stores segments, not values!
uint8_t currdigit = 0;        // which digit we are currently multiplexing

// This table allow us to index between what digit we want to light up
// and what the pin number is on the MAX6921 see the .h for values.
// Stored in ROM (PROGMEM) to save RAM
const uint8_t digittable[] PROGMEM = {
  DIG_9, DIG_8, DIG_7, DIG_6, DIG_5, DIG_4, DIG_3, DIG_2, DIG_1
};
PGM_P digittable_p PROGMEM = digittable;

// This table allow us to index between what segment we want to light up
// and what the pin number is on the MAX6921 see the .h for values.
// Stored in ROM (PROGMEM) to save RAM
const uint8_t segmenttable[] PROGMEM = {
  SEG_H, SEG_G,  SEG_F,  SEG_E,  SEG_D,  SEG_C,  SEG_B,  SEG_A 
};
PGM_P segmenttable_p PROGMEM = segmenttable;

// muxdiv and MUX_DIVIDER divides down a high speed interrupt (31.25KHz)
// down so that we can refresh at about 100Hz (31.25KHz / 300)
// We refresh the entire display at 100Hz so each digit is updated
// 100Hz/DISPLAYSIZE
uint16_t muxdiv = 0;
#define MUX_DIVIDER (300 / DISPLAYSIZE)

// Likewise divides 100Hz down to 1Hz for the alarm beeping
uint16_t alarmdiv = 0;
#define ALARM_DIVIDER 100

// How long we have been snoozing
uint16_t snoozetimer = 0;

// We have a non-blocking delay function, milliseconds is updated by
// an interrupt
volatile uint16_t milliseconds = 0;
void delayms(uint16_t ms) {
  sei();

  milliseconds = 0;
  while (milliseconds < ms);
}

// When the alarm is going off, pressing a button turns on snooze mode
// this sets the snoozetimer off in MAXSNOOZE seconds - which turns on
// the alarm again
void setsnooze(void) {
#ifdef FEATURE_SETSNOOZETIME
  snoozetimer = eeprom_read_byte((uint8_t *)EE_SNOOZE);
  snoozetimer *= 60; // convert minutes to seconds
#else
  snoozetimer = MAXSNOOZE;
#endif
  DEBUGP("snooze");
  display_str("snoozing");
  displaymode = SHOW_SNOOZE;
  delayms(1000);
  displaymode = SHOW_TIME;
}

// we reset the watchdog timer 
void kickthedog(void) {
  wdt_reset();
}

// called @ (F_CPU/256) = ~30khz (31.25 khz)
SIGNAL (SIG_OVERFLOW0) {
  // allow other interrupts to go off while we're doing display updates
  sei();

  // kick the dog
  kickthedog();

  // divide down to 100Hz * digits
  muxdiv++;
  if (muxdiv < MUX_DIVIDER)
    return;
  muxdiv = 0;
  // now at 100Hz * digits

  // ok its not really 1ms but its like within 10% :)
  milliseconds++;

  // Cycle through each digit in the display
  if (currdigit >= DISPLAYSIZE)
    currdigit = 0;

  // Set the current display's segments
  setdisplay(currdigit, display[currdigit]);
  // and go to the next
  currdigit++;

  // check if we should have the alarm on
  if (alarming && !snoozetimer) {
    alarmdiv++;
    if (alarmdiv > ALARM_DIVIDER) {
      alarmdiv = 0;
    } else {
      return;
    }
    // This part only gets reached at 1Hz

    // This sets the buzzer frequency
    ICR1 = 250;
    OCR1A = OCR1B = ICR1/2;

    // ok alarm is ringing!
    if (alarming & 0xF0) { // top bit indicates pulsing alarm state
      alarming &= ~0xF0;
      TCCR1B &= ~_BV(CS11); // turn buzzer off!
    } else {
      alarming |= 0xF0;
      TCCR1B |= _BV(CS11); // turn buzzer on!
    }
  }
  
}


// We use the pin change interrupts to detect when buttons are pressed

// These store the current button states for all 3 buttons. We can 
// then query whether the buttons are pressed and released or pressed
// This allows for 'high speed incrementing' when setting the time
volatile uint8_t last_buttonstate = 0, just_pressed = 0, pressed = 0;
volatile uint8_t buttonholdcounter = 0;

// This interrupt detects switches 1 and 3
SIGNAL(SIG_PIN_CHANGE2) {
  // allow interrupts while we're doing this
  PCMSK2 = 0;
  sei();
  // kick the dog
  kickthedog();

  if (! (PIND & _BV(BUTTON1))) {
    // button1 is pressed
    if (! (last_buttonstate & 0x1)) { // was not pressed before
      delayms(10);                    // debounce
      if (PIND & _BV(BUTTON1))        // filter out bounces
      {
        PCMSK2 = _BV(PCINT21) | _BV(PCINT20);
         return;
      }
      tick();                         // make a noise
      // check if we will snag this button press for snoozing
      if (alarming) {
        // turn on snooze
        setsnooze();
        PCMSK2 = _BV(PCINT21) | _BV(PCINT20);
        return;
      }
      last_buttonstate |= 0x1;
      just_pressed |= 0x1;
      DEBUGP("b1");
    }
  } else {
    last_buttonstate &= ~0x1;
  }

  if (! (PIND & _BV(BUTTON3))) {
    // button3 is pressed
    if (! (last_buttonstate & 0x4)) { // was not pressed before
      delayms(10);                    // debounce
      if (PIND & _BV(BUTTON3))        // filter out bounces
      {
        PCMSK2 = _BV(PCINT21) | _BV(PCINT20);
        return;
      }
      buttonholdcounter = 2;          // see if we're press-and-holding
      while (buttonholdcounter) {
        if (PIND & _BV(BUTTON3)) {        // released
          tick();                         // make a noise
          last_buttonstate &= ~0x4;
          // check if we will snag this button press for snoozing
          if (alarming) {
            // turn on snooze
            setsnooze();
            PCMSK2 = _BV(PCINT21) | _BV(PCINT20);
            return;
          }
          DEBUGP("b3");
          just_pressed |= 0x4;
          PCMSK2 = _BV(PCINT21) | _BV(PCINT20);
          return;
        }
      }
      last_buttonstate |= 0x4;
      pressed |= 0x4;                 // held down
    }
  } else {
    pressed = 0;                      // button released
    last_buttonstate &= ~0x4;
  }
  PCMSK2 = _BV(PCINT21) | _BV(PCINT20);
}

// Just button #2
SIGNAL(SIG_PIN_CHANGE0) {
  PCMSK0 = 0;
  sei();
  if (! (PINB & _BV(BUTTON2))) {
    // button2 is pressed
    if (! (last_buttonstate & 0x2)) { // was not pressed before
      delayms(10);                    // debounce
      if (PINB & _BV(BUTTON2))        // filter out bounces
      {
        PCMSK0 = _BV(PCINT0);
        return;
      }
      tick();                         // make a noise
      // check if we will snag this button press for snoozing
      if (alarming) {
        setsnooze(); 	// turn on snooze
        PCMSK0 = _BV(PCINT0);
        return;
      }
      last_buttonstate |= 0x2;
      just_pressed |= 0x2;
      DEBUGP("b2");
    }
  } else {
    last_buttonstate &= ~0x2;
  }
  PCMSK0 = _BV(PCINT0);
}
// This variable keeps track of whether we have not pressed any
// buttons in a few seconds, and turns off the menu display
volatile uint8_t timeoutcounter = 0;

#ifdef FEATURE_DST
  
void checkdstrule(uint8_t day1, uint8_t day2, uint8_t dayofweek, uint8_t month, uint8_t hour, uint8_t type)
{
  //Set day1=day2 and dayofweek=8 if a rule involves something like 1st of january or 29th of march.
  //If a rule involves 1st sunday or last sunday of a month, then set dayofweek to 0,
  //and day to the range that the first sunday can appear in for that month.
  //If month = 2, and you are doing last sunday, make sure to account for leapyears.
  //Finally, you must specify which hour the spring ahead/fall back takes effect.
  //The DST correction flag is set once all of these conditions are met for
  //fall back, and is NOT cleared till the next day.
  //Type = 0 - Spring ahead
  //Type = 1 - Fall back.
  if(!dst_set)
  {
    if((date_d >= day1) && (date_d <= day2) && (date_m == month) && (time_h == hour))
    {
      if(dayofweek == 7)
      {
        if(type==0)
        {
          time_h++;
        }
        else if(type==1)
        {
          dst_set=1;
          time_h--;
        }
        countdown_init(); //recalculate countdown
        return; //Ignore day of week. Set day1 = day2 in this case.
      }
      if(dayofweek == dotw())
      {
        if(type==0)
        {
          time_h++;
        }
        else if(type==1)
        {
          dst_set=1;
          time_h--;
        }
        countdown_init(); //recalculate countdown
        return; //Used for a range of days,  typically for first sunday, or last sunday type rules.
      }
    }
  }
  else
  {
    //Exactly one day after the set time, we need to clear the dst_set flag, so
    //that the dst can fall back again, next year.
    if(((date_d < day1) || (date_d > day2) || (date_m != month)) && (type == 1))
    {
      dst_set=0;
    }
    else if (type==1)
    {
      if((dayofweek != dotw()) && (dayofweek != 7))
      {
        dst_set=0;
      }
    }
  }
  //Didn't meet any of the rules. return false as a result.
  return;
}

#endif

// this goes off once a second
SIGNAL (TIMER2_OVF_vect) {
  CLKPR = _BV(CLKPCE);  //MEME
  CLKPR = 0;

  //if(displaymode!=SHOW_TIME)
    time_s++;             // one second has gone by
    cd_s--;
  //else
  //  time_s=60;  //Accellerate for debugging. Uncomment these lines only for that purpose.

  // a minute!
  if (time_s >= 60) {
    time_s = 0;
    time_m++;
  }
  if (cd_s < 0) {
    cd_s = 59;
    cd_m--;
  }

  // an hour...
  if (time_m >= 60) {
    time_m = 0;
    time_h++; 
#ifdef FEATURE_DST
  #ifdef DST_GERMANY
    if(dst_on == DST_GERMANY)
    {
      checkdstrule(25,31,0,3,2,0);  //Spring ahead Last sunday of march.
      checkdstrule(25,31,0,10,2,1); //Fall back Last sunday of October.
      //We previously reset this flag here.  Unfortunately, this created a massive
      //showstopping bug, where the icetube was always 4:xx:xx AM on the day the
      //DST changeover takes effect.  Anyhow, the changes to the parameters of the
      //check function, and actually checking the hour inside of it, fixed it.
    }
  #endif
  #ifdef DST_USA
    if(dst_on == DST_USA)  //Check daylight saving time.
    {
      checkdstrule(8,14,0,3,2,0); //Spring ahead Second sunday of March
      checkdstrule(1,7,0,11,2,1); //Fall back First sunday of November
    }
  #endif
#endif
    // lets write the time to the EEPROM
    eeprom_write_byte((uint8_t *)EE_HOUR, time_h);
    eeprom_write_byte((uint8_t *)EE_MIN, time_m);
  }
  if (cd_m < 0) {
    cd_m = 59;
    cd_h--; 
  }
  
  // a day....
  if (time_h >= 24) {
    time_h = 0;
    date_d++;
    eeprom_write_byte((uint8_t *)EE_DAY, date_d);
  }
  if (cd_h < 0) {
    cd_h = 23;
    cd_d--;
  }

  /*
  if (! sleepmode) {
    uart_putw_dec(time_h);
    uart_putchar(':');
    uart_putw_dec(time_m);
    uart_putchar(':');
    uart_putw_dec(time_s);
    putstring_nl("");
  }
  */

  // a full month!
  // we check the leapyear and date to verify when its time to roll over months
  if ((date_d > 31) ||
      ((date_d == 31) && ((date_m == 4)||(date_m == 6)||(date_m == 9)||(date_m == 11))) ||
      ((date_d == 30) && (date_m == 2)) ||
      ((date_d == 29) && (date_m == 2) && !leapyear(2000+date_y))) {
    date_d = 1;
    date_m++;
    eeprom_write_byte((uint8_t *)EE_MONTH, date_m);
  }
  
  // HAPPY NEW YEAR!
  if (date_m >= 13) {
    date_y++;
    date_m = 1;
    eeprom_write_byte((uint8_t *)EE_YEAR, date_y);
  }
  
  // If we're in low power mode we should get out now since the display is off
  if (sleepmode)
    return;
   

  if (displaymode == SHOW_TIME) {
    if (timeunknown && (time_s % 2)) {
      display_str("        ");
    } else {
      display_time(time_h, time_m, time_s);
    }
    if (alarm_on)
      display[0] |= 0x2;
    else 
      display[0] &= ~0x2;
    
  }
  if (displaymode == SHOW_COUNTDOWN) {
    if (timeunknown && (time_s % 2)) {
      display_str("        ");
    } else {
      display_countdown(cd_d, cd_h, cd_m, cd_s);
    }
    if (alarm_on)
      display[0] |= 0x2;
    else 
      display[0] &= ~0x2;
    
  }
  if (alarm_on && (alarm_h == time_h) && (alarm_m == time_m) && (time_s == 0)) {
    DEBUGP("alarm on!");
    alarming = 1;
    snoozetimer = 0;
  }

#ifdef FEATURE_AUTODIM
  dimmer_update();
#endif

  if (timeoutcounter)
    timeoutcounter--;
  if (buttonholdcounter)
    buttonholdcounter--;
  if (snoozetimer) {
    snoozetimer--;
    if (snoozetimer % 2) 
      display[0] |= 0x2;
    else
      display[0] &= ~0x2;
  }
}

SIGNAL(SIG_INTERRUPT0) {
  EIMSK = 0;  //Disable this interrupt while we are processing it.
  DEBUGP("i");
  uint8_t x = ALARM_PIN & _BV(ALARM);
  sei();
  delayms(10); // wait for debouncing
  if (x != (ALARM_PIN & _BV(ALARM)))
  {
    EIMSK = _BV(INT0);
    return;
  }
  setalarmstate();
  EIMSK = _BV(INT0);  //And reenable it before exiting.
}



SIGNAL(SIG_COMPARATOR) {
  //DEBUGP("COMP");
  if (ACSR & _BV(ACO)) {
    //DEBUGP("HIGH");
    if (!sleepmode) {
      VFDSWITCH_PORT |= _BV(VFDSWITCH); // turn off display
      VFDCLK_PORT &= ~_BV(VFDCLK) & ~_BV(VFDDATA); // no power to vfdchip
      BOOST_PORT &= ~_BV(BOOST); // pull boost fet low
      SPCR  &= ~_BV(SPE); // turn off spi
      if (restored) {
        eeprom_write_byte((uint8_t *)EE_MIN, time_m);
        eeprom_write_byte((uint8_t *)EE_SEC, time_s);
      }
      DEBUGP("z");
      TCCR0B = 0; // no boost
      volume = 0; // low power buzzer
      PCICR = 0;  // ignore buttons
#ifdef FEATURE_AUTODIM
      DIMMER_POWER_PORT &= ~_BV(DIMMER_POWER_PIN); // no power to photoresistor
#endif

      app_start();
    }
  } else {
    //DEBUGP("LOW");
    if (sleepmode) {
      if (restored) {
        eeprom_write_byte((uint8_t *)EE_MIN, time_m);
        eeprom_write_byte((uint8_t *)EE_SEC, time_s);
      }
      DEBUGP("WAKERESET"); 
      app_start();
    }
  }
}
/*********************** Main app **********/
void initeeprom(void) {
  if(eeprom_read_byte((uint8_t *)EE_INIT)!=1)
  {
    eeprom_write_byte((uint8_t*)EE_INIT, 1);  //Initialize one time.
    eeprom_write_byte((uint8_t*)EE_YEAR, 11);
    eeprom_write_byte((uint8_t*)EE_MONTH, 1);
    eeprom_write_byte((uint8_t*)EE_DAY, 1);   //Jan 1, 2000
    eeprom_write_byte((uint8_t*)EE_HOUR, 0);
    eeprom_write_byte((uint8_t*)EE_MIN, 0);
    eeprom_write_byte((uint8_t*)EE_SEC, 0);   //00:00:00 (24Hour), 12:00:00 AM (12Hour)
    eeprom_write_byte((uint8_t*)EE_ALARM_HOUR, 10);
    eeprom_write_byte((uint8_t*)EE_ALARM_MIN, 0);   //Alarm 10:00:00/10:00:00AM
    eeprom_write_byte((uint8_t*)EE_BRIGHT, 30);     //Brightness Level 30
    eeprom_write_byte((uint8_t*)EE_VOLUME, 0);      //Volume Low
    eeprom_write_byte((uint8_t*)EE_REGION, REGION_US);  //12 Hour mode
    eeprom_write_byte((uint8_t*)EE_SNOOZE, 10);     //10 Minute Snooze. (If compiled in.)
    eeprom_write_byte((uint8_t*)EE_DST, 0);         //No Daylight Saving Time
    eeprom_write_byte((uint8_t*)EE_CD_YEAR, 11);         //Countdown to Date
    eeprom_write_byte((uint8_t*)EE_CD_MONTH, 1); 
    eeprom_write_byte((uint8_t*)EE_CD_DAY, 0); 
    eeprom_write_byte((uint8_t*)EE_CD_HOUR, 0); 
    eeprom_write_byte((uint8_t*)EE_CD_MIN, 0); 
    beep(3000,2);                                   //And acknowledge EEPROM written.
  }

}

#ifdef FEATURE_TESTMODE
void testmode(uint8_t force) {
  uint8_t seconds = time_s;
  uint8_t alarm_state = 0;
  uint8_t testdigit=0;
  uint8_t testvalue=0;
  uint8_t testexit=5;
  //uint8_t dim_on, dim_status;
  uint8_t i;
  #ifdef FEATURE_AUTODIM
  uint8_t j=0;
  uint16_t k;
  uint8_t blevel=0xFF;
  #endif
  
  if(!force)
  {
    if ((PIND & _BV(BUTTON1))) {
      return;
    }
    if((PIND & _BV(BUTTON3))) {
        return;
    }
  }
  beep(2000,1);
  beep(3000,1);
  beep(4000,1);
  while (!(PIND & _BV(BUTTON1)));
  while (!(PIND & _BV(BUTTON3)));
  for(i=0;i<9;i++)
    display[i] = 0;
  displaymode = TESTMODE;
  while(1) {
    kickthedog();
    
    #ifdef FEATURE_AUTODIM
    if(j!=0)
    {
      k=dimmer_level;
      display[8] = pgm_read_byte(numbertable_p + (k % 10));
      k/=10;
      display[7] = pgm_read_byte(numbertable_p + (k % 10));
      k/=10;
      display[6] = pgm_read_byte(numbertable_p + (k % 10));
      display[5] = pgm_read_byte(numbertable_p + (k / 10));
    }
    #endif
    if(just_pressed&1)
    {
      beep(2000,1);
      just_pressed &= ~1;
      #ifdef FEATURE_AUTODIM
      if(j==0)
      {
      #endif
        if((testdigit==0))
        {
          if(display[0]==0)
            display[0]=1;
          else if (display[0]==1)
            display[0]=2;
          else
          {
            display[0]=0;
            testdigit++;
            display[testdigit]=1;
          }
        }
        else
        {
          display[testdigit]<<=1;
          if(display[testdigit]==0)
          {
            testdigit++;
            if(testdigit==9)
              testdigit=0;
            else
              display[testdigit]=1;
          }
        }
      #ifdef FEATURE_AUTODIM
      }
      #endif
        
    }
    if(just_pressed&2)
    {
      beep(2500,1);
      just_pressed &= ~2;
      #ifdef FEATURE_AUTODIM
      if(!j)
      {
        blevel = brightness_level;
        brightness_level = 0;
        dimmer_update();
        j = 1;
        //dimmer_update();
      }
      #endif
    }
    if(just_pressed&4)
    {
      beep(3000,1);
      just_pressed &= ~4;
      #ifdef FEATURE_AUTODIM
      if(j)
      {
        if(blevel!=0xFF)
        {
          brightness_level = blevel;
          set_vfd_brightness(blevel);
        }
        j=0;
        for(i=0;i<9;i++)
          display[i] = 0;
        //set_vfd_brightness(brightness_level);
      }
      #endif
      
    }
    if(seconds != time_s)
    {
      time_s = seconds;
      tick();
      if(!(PIND & _BV(BUTTON1))) {
        if(!(PIND & _BV(BUTTON3))) {
          testexit--;
          switch(testexit)
          {
            case 4:
              testvalue = display[testdigit];
              display_str("e        ");
              break;
            case 3:
              display_str("ex");
              break;
            case 2:
              display_str("exi");
              break;
            case 1:
              display_str("exit");
              break;
            default:
              display_str("exiting");
              break;
          }
          if(testexit==0)
          {
            while (!(PIND & _BV(BUTTON1)));
            while (!(PIND & _BV(BUTTON3)));
            beep(4000,1);
            beep(3000,1);
            beep(2000,1);
            #ifdef FEATURE_AUTODIM
            if(blevel!=0xFF)
            {
              if(blevel)
              {
                brightness_level = blevel;
                set_vfd_brightness(blevel);
              }
              else
              {
                dimmer_update();
              }
            }
            #endif
            return;
          }
        }
        else
        {
          if(testexit<5)
          {
            for(i=0;i<9;i++)
              display[i] = 0;
            display[testdigit] = testvalue;
          }
          testexit=5;
        }
      }
      else
      {
        if(testexit<5)
        {
          for(i=0;i<9;i++)
            display[i] = 0;
          display[testdigit] = testvalue;
        }
        testexit=5;
      }
    }
    if(alarm_state != alarm_on)
    {
      alarm_state = alarm_on;
      beep(4000,1 + alarm_on);
    }
  }
}
#endif

uint32_t t;

void gotosleep(void) {
  // battery
  //if (sleepmode) //already asleep?
  //  return;
  //DEBUGP("sleeptime");
  
  sleepmode = 1;
  VFDSWITCH_PORT |= _BV(VFDSWITCH); // turn off display
  SPCR  &= ~_BV(SPE); // turn off spi
  VFDCLK_PORT &= ~_BV(VFDCLK) & ~_BV(VFDDATA); // no power to vfdchip
  BOOST_PORT &= ~_BV(BOOST); // pull boost fet low
  TCCR0B = 0; // no boost
  volume = 0; // low power buzzer
  PCICR = 0;  // ignore buttons
#ifdef FEATURE_AUTODIM
  DIMMER_POWER_PORT &= ~_BV(DIMMER_POWER_PIN); // no power to photoresistor
#endif

  // sleep time!
  //beep(3520, 1);
  //beep(1760, 1);
  //beep(880, 1);
  // turn beeper off
  PORTB &= ~_BV(SPK1) & ~_BV(SPK2); 
  
  // turn off pullups
  PORTD &= ~_BV(BUTTON1) & ~_BV(BUTTON3);
  PORTB &= ~_BV(BUTTON2);
  DDRD &= ~_BV(BUTTON1) & ~_BV(BUTTON3);
  DDRB &= ~_BV(BUTTON2);
  ALARM_PORT &= ~_BV(ALARM);
  ALARM_DDR &= ~_BV(ALARM);
  

  // reduce the clock speed
  CLKPR = _BV(CLKPCE);
  CLKPR = _BV(CLKPS3);
  
  //  PPR |= _BV(PRUSART0) | _BV(PRADC) | _BV(PRSPI) | _BV(PRTIM1) | _BV(PRTIM0) | _BV(PRTWI);
  PORTC |= _BV(4);  // sleep signal
  SMCR |= _BV(SM1) | _BV(SM0) | _BV(SE); // sleep mode
  asm("sleep"); 
  CLKPR = _BV(CLKPCE);
  CLKPR = 0;
  PORTC &= ~_BV(4);
}

#if 0 // unused function - no point letting it take up space
 void wakeup(void) {
   if (!sleepmode)
     return;
   CLKPR = _BV(CLKPCE);
   CLKPR = 0;
   DEBUGP("waketime");
   sleepmode = 0;
   // plugged in
   // wait to verify
   _delay_ms(20);
   if (ACSR & _BV(ACO)) 
     return;
   
   // turn on pullups
   initbuttons();

#ifdef FEATURE_AUTODIM
   dimmer_init();
#endif

   // turn on boost
   brightness_level = eeprom_read_byte((uint8_t *)EE_BRIGHT);
   boost_init(brightness_level);

   // turn on vfd control
   vfd_init();

   // turn on display
   VFDSWITCH_PORT &= ~_BV(VFDSWITCH); 
   VFDBLANK_PORT &= ~_BV(VFDBLANK);
   volume = eeprom_read_byte((uint8_t *)EE_VOLUME); // reset
   
   speaker_init();

   kickthedog();

   setalarmstate();

   // wake up sound
   beep(880, 1);
   beep(1760, 1);
   beep(3520, 1);

   kickthedog();
 }
#endif


void initbuttons(void) {
    DDRB =  _BV(VFDCLK) | _BV(VFDDATA) | _BV(SPK1) | _BV(SPK2);
    DDRD = _BV(BOOST) | _BV(VFDSWITCH);
    DDRC = _BV(VFDLOAD) | _BV(VFDBLANK);
    PORTD = _BV(BUTTON1) | _BV(BUTTON3) | _BV(ALARM);
    PORTB = _BV(BUTTON2);

    PCICR = _BV(PCIE0) | _BV(PCIE2);
    PCMSK0 = _BV(PCINT0);
    PCMSK2 = _BV(PCINT21) | _BV(PCINT20);    
}



int main(void) {
  //  uint8_t i;
  uint8_t mcustate;
  
#ifdef FEATURE_TESTMODE
  uint8_t test=0;
  
  if (! (PIND & _BV(BUTTON1))) {
    if(!(PIND & _BV(BUTTON3))) {
      test=1;
    }
  }
#endif
  // turn boost off
  TCCR0B = 0;
  BOOST_DDR |= _BV(BOOST);
  BOOST_PORT &= ~_BV(BOOST); // pull boost fet low

  // check if we were reset
  mcustate = MCUSR;
  MCUSR = 0;

  wdt_disable();
  // now turn it back on... 2 second time out
  //WDTCSR |= _BV(WDP0) | _BV(WDP1) | _BV(WDP2);
  //WDTCSR = _BV(WDE);
  wdt_enable(WDTO_2S);
  kickthedog();

  // we lost power at some point so lets alert the user
  // that the time may be wrong (the clock still works)
  timeunknown = 1;

  // have we read the time & date from eeprom?
  restored = 0;

  // setup uart
  uart_init(BRRL_192);
  //DEBUGP("VFD Clock");
  DEBUGP("!");

  //DEBUGP("turning on anacomp");
  // set up analog comparator
  ACSR = _BV(ACBG) | _BV(ACIE); // use bandgap, intr. on toggle!
  // settle!
  if (ACSR & _BV(ACO)) {
    // hmm we should not interrupt here
    ACSR |= _BV(ACI);

    // even in low power mode, we run the clock 
    DEBUGP("clock init");
    clock_init();  

  } else {
    // we aren't in low power mode so init stuff

    // init io's
    initbuttons();
    
    VFDSWITCH_PORT &= ~_BV(VFDSWITCH);
    
    DEBUGP("turning on buttons");
    // set up button interrupts
    DEBUGP("turning on alarmsw");
    // set off an interrupt if alarm is set or unset
    EICRA = _BV(ISC00);
    EIMSK = _BV(INT0);
  
    displaymode = SHOW_TIME;
    DEBUGP("vfd init");
    vfd_init();
    
    DEBUGP("speaker init");
    speaker_init();
    
    DEBUGP("eeprom init");  //Reset eeprom to defaults, if completely blank.
    initeeprom();
#ifdef FEATURE_AUTODIM
    dimmer_init();
#endif

    DEBUGP("boost init");
    brightness_level = eeprom_read_byte((uint8_t *)EE_BRIGHT);
    boost_init(brightness_level);
    sei();

    region = eeprom_read_byte((uint8_t *)EE_REGION); 
#ifdef FEATURE_DST
    dst_on = eeprom_read_byte((uint8_t *)EE_DST);
#endif
    
    beep(4000, 1);

    DEBUGP("clock init");
    clock_init();  

    DEBUGP("alarm init");
    setalarmstate();
#ifdef FEATURE_TESTMODE
    if(test)
    {
      displaymode = TESTMODE;
      testmode(0);
      displaymode = SHOW_TIME;
    }
#endif
  }
  DEBUGP("done");
  while (1) {
    //_delay_ms(100);
    kickthedog();
    //uart_putc_hex(ACSR);
    if (ACSR & _BV(ACO)) {
      // DEBUGP("SLEEPYTIME");
      gotosleep();
      continue;
    }
    //DEBUGP(".");
    if (just_pressed & 0x1) {
      just_pressed = 0;
      switch(displaymode) {
      case (SHOW_TIME):
        displaymode = SHOW_COUNTDOWN;
        break;
      case (SHOW_COUNTDOWN):
        displaymode = SET_ALARM;
        display_str("set alarm");
        set_alarm();
        break;
      case (SET_ALARM):
        displaymode = SET_TIME;
        display_str("set time");
        set_time();
        timeunknown = 0;
        break;
      case (SET_TIME):
        displaymode = SET_DATE;
        display_str("set date");
        set_date();
        break;
      case (SET_DATE):
        displaymode = SET_COUNTDOWN_DATE;
        display_str("cnt date");
        set_cd_date();
        break;
      case (SET_COUNTDOWN_DATE):
        displaymode = SET_COUNTDOWN_TIME;
        display_str("cnt time");
        set_cd_time();
        break;
      case (SET_COUNTDOWN_TIME):
        displaymode = SET_BRIGHTNESS;
        display_str("set brit");
        set_brightness();
        break;
      case (SET_BRIGHTNESS):
        displaymode = SET_VOLUME;
        display_str("set vol ");
        set_volume();
        break;
      case (SET_VOLUME):
        displaymode = SET_REGION;
        display_str("set regn");
        set_region();
        break;
      case (SET_REGION):    	
#ifdef FEATURE_DST
        displaymode = SET_DAYLIGHTSAVINGTIME;
        display_str("set dst ");
        set_dst();
        break;
      case (SET_DAYLIGHTSAVINGTIME):
#endif
#ifdef FEATURE_TESTMODE
        displaymode = TESTMODE;
        display_str("testmode");
        set_test();
        break;
      case (TESTMODE):
#endif
#ifdef FEATURE_SETSNOOZETIME  
        displaymode = SET_SNOOZE;
        display_str("set snoz");
        set_snooze();
        break;
      case (SET_SNOOZE):
#endif
      default:
        displaymode = SHOW_TIME;
      } // end of switch
    } else if ((just_pressed & 0x2) || (just_pressed & 0x4)) {
      just_pressed = 0;
      displaymode = NONE;
      display_date(DAY);

      kickthedog();
      delayms(1500);
      kickthedog();

      displaymode = SHOW_TIME;     
    } 
  }
}

/**************************** SUB-MENUS *****************************/

void set_alarm(void) 
{
  uint8_t mode;
  uint8_t hour, min, sec;
    
  hour = min = sec = 0;
  mode = SHOW_MENU;

  hour = alarm_h;
  min = alarm_m;
  sec = 0;
  
  timeoutcounter = INACTIVITYTIMEOUT;
  
  while (1) {
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      alarm_h = hour;
      alarm_m = min;
      eeprom_write_byte((uint8_t *)EE_ALARM_HOUR, alarm_h);    
      eeprom_write_byte((uint8_t *)EE_ALARM_MIN, alarm_m);    
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // ok now its selected
        mode = SET_HOUR;
        display_alarm(hour, min);
        display[1] |= 0x1;
        display[2] |= 0x1;	
      } else if (mode == SET_HOUR) {
        mode = SET_MIN;
        display_alarm(hour, min);
        display[4] |= 0x1;
        display[5] |= 0x1;
      } else {
        // done!
        alarm_h = hour;
        alarm_m = min;
        eeprom_write_byte((uint8_t *)EE_ALARM_HOUR, alarm_h);    
        eeprom_write_byte((uint8_t *)EE_ALARM_MIN, alarm_m);    
        displaymode = SHOW_TIME;
        return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;

      if (mode == SET_HOUR) {
        hour = (hour+1) % 24;
        display_alarm(hour, min);
        display[1] |= 0x1;
        display[2] |= 0x1;
      }
      if (mode == SET_MIN) {
        min = (min+1) % 60;
        display_alarm(hour, min);
        display[4] |= 0x1;
        display[5] |= 0x1;
      }

      if (pressed & 0x4)
        delayms(75);
    }
  }
}

void set_time(void) 
{
  uint8_t mode;
  uint8_t hour, min, sec;
    
  hour = time_h;
  min = time_m;
  sec = time_s;
  mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;
  
  while (1) {
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
        hour = time_h;
        min = time_m;
        sec = time_s;

        // ok now its selected
        mode = SET_HOUR;
        display_time(hour, min, sec);
        display[1] |= 0x1;
        display[2] |= 0x1;	
      } else if (mode == SET_HOUR) {
        mode = SET_MIN;
        display_time(hour, min, sec);
        display[4] |= 0x1;
        display[5] |= 0x1;
      } else if (mode == SET_MIN) {
        mode = SET_SEC;
        display_time(hour, min, sec);
        display[7] |= 0x1;
        display[8] |= 0x1;
      } else {
        // done!
        time_h = hour;
        time_m = min;
        time_s = sec;
#ifdef FEATURE_DST
  #ifdef DST_USA
  if(dst_on==DST_USA)
    dst_set = (hour <= 2) ? 1 : 0;
  #endif
  #ifdef DST_GERMANY
  if(dst_on==DST_GERMANY)
    dst_set = (hour <= 2) ? 1 : 0;
  #endif
#endif
        displaymode = SHOW_TIME;
        return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      
      if (mode == SET_HOUR) {
        hour = (hour+1) % 24;
        display_time(hour, min, sec);
        display[1] |= 0x1;
        display[2] |= 0x1;
        time_h = hour;
        eeprom_write_byte((uint8_t *)EE_HOUR, time_h);    
        countdown_init(); //recalculate countdown
      }
      if (mode == SET_MIN) {
        min = (min+1) % 60;
        display_time(hour, min, sec);
        display[4] |= 0x1;
        display[5] |= 0x1;
        eeprom_write_byte((uint8_t *)EE_MIN, time_m);
        time_m = min;
        countdown_init(); //recalculate countdown
      }
      if ((mode == SET_SEC) ) {
        sec = (sec+1) % 60;
        display_time(hour, min, sec);
        display[7] |= 0x1;
        display[8] |= 0x1;
        time_s = sec;
        countdown_init(); //recalculate countdown
      }
      
      if (pressed & 0x4)
        delayms(75);
    }
  }
}


void set_date(void) {
  uint8_t mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;;  

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {

      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // start!
        if (region == REGION_US) {
          mode = SET_MONTH;
        }
        else {
          DEBUGP("Set day");
          mode = SET_DAY;
        }
        display_date(DATE);
        display[1] |= 0x1;
        display[2] |= 0x1;
      } else if (((mode == SET_MONTH) && (region == REGION_US)) ||
                 ((mode == SET_DAY) && (region == REGION_EU))) {
        if (region == REGION_US)
          mode = SET_DAY;
        else
          mode = SET_MONTH;
        display_date(DATE);
        display[4] |= 0x1;
        display[5] |= 0x1;
      } else if (((mode == SET_DAY) && (region == REGION_US)) ||
        ((mode == SET_MONTH) && (region == REGION_EU))) {
        mode = SET_YEAR;
        display_date(DATE);
        display[7] |= 0x1;
        display[8] |= 0x1;
      } else {
        displaymode = NONE;
        display_date(DATE);
        delayms(1500);
        displaymode = SHOW_TIME;
        return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      if (mode == SET_MONTH) {
        date_m++;
        if (date_m >= 13)
          date_m = 1;
        display_date(DATE);
        if (region == REGION_US) {
          display[1] |= 0x1;
          display[2] |= 0x1;
        } else {
          display[4] |= 0x1;
          display[5] |= 0x1;
        }
        eeprom_write_byte((uint8_t *)EE_MONTH, date_m);  
        countdown_init(); //recalculate countdown
      }
      if (mode == SET_DAY) {
        date_d++;
        if (date_d > 31)
          date_d = 1;
        display_date(DATE);

        if (region == REGION_EU) {
          display[1] |= 0x1;
          display[2] |= 0x1;
        } else {
          display[4] |= 0x1;
          display[5] |= 0x1;
        }
        eeprom_write_byte((uint8_t *)EE_DAY, date_d);    
        countdown_init(); //recalculate countdown
      }
      if (mode == SET_YEAR) {
        date_y++;
        date_y %= 100;
        display_date(DATE);
        display[7] |= 0x1;
        display[8] |= 0x1;
        eeprom_write_byte((uint8_t *)EE_YEAR, date_y);    
        countdown_init(); //recalculate countdown
      }

      if (pressed & 0x4)
        delayms(60);
    }
  }
}


void set_cd_date(void) {
  uint8_t mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;;  

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {

      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // start!
        if (region == REGION_US) {
          mode = SET_MONTH;
        }
        else {
          DEBUGP("Set day");
          mode = SET_DAY;
        }
        display_cd_date();
        display[1] |= 0x1;
        display[2] |= 0x1;
      } else if (((mode == SET_MONTH) && (region == REGION_US)) ||
                 ((mode == SET_DAY) && (region == REGION_EU))) {
        if (region == REGION_US)
          mode = SET_DAY;
        else
          mode = SET_MONTH;
        display_cd_date();
        display[4] |= 0x1;
        display[5] |= 0x1;
      } else if (((mode == SET_DAY) && (region == REGION_US)) ||
        ((mode == SET_MONTH) && (region == REGION_EU))) {
        mode = SET_YEAR;
        display_cd_date();
        display[7] |= 0x1;
        display[8] |= 0x1;
      } else {
        displaymode = NONE;
        display_cd_date();
        delayms(1500);
        displaymode = SHOW_TIME;
        return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      if (mode == SET_MONTH) {
        end_month++;
        if (end_month >= 13)
          end_month = 1;
        display_cd_date();
        if (region == REGION_US) {
          display[1] |= 0x1;
          display[2] |= 0x1;
        } else {
          display[4] |= 0x1;
          display[5] |= 0x1;
        }
        eeprom_write_byte((uint8_t *)EE_CD_MONTH, end_month);  
        countdown_init(); //recalculate countdown
      }
      if (mode == SET_DAY) {
        end_day++;
        if (end_day > 31)
          end_day = 1;
        display_cd_date();

        if (region == REGION_EU) {
          display[1] |= 0x1;
          display[2] |= 0x1;
        } else {
          display[4] |= 0x1;
          display[5] |= 0x1;
        }
        eeprom_write_byte((uint8_t *)EE_CD_DAY, end_day);    
        countdown_init(); //recalculate countdown
      }
      if (mode == SET_YEAR) {
        end_year++;
        end_year %= 100;
        display_cd_date();
        display[7] |= 0x1;
        display[8] |= 0x1;
        eeprom_write_byte((uint8_t *)EE_CD_YEAR, end_year);    
        countdown_init(); //recalculate countdown
      }

      if (pressed & 0x4)
        delayms(60);
    }
  }
}


void set_cd_time(void) 
{
  uint8_t mode;
  uint8_t hour, min, sec;
    
  hour = min = sec = 0;
  mode = SHOW_MENU;

  hour = end_hour;
  min = end_min;
  sec = 0;
  
  timeoutcounter = INACTIVITYTIMEOUT;
  
  while (1) {
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
        displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // ok now its selected
        mode = SET_HOUR;
        display_alarm(hour, min);
        display[1] |= 0x1;
        display[2] |= 0x1;	
      } else if (mode == SET_HOUR) {
        mode = SET_MIN;
        display_alarm(hour, min);
        display[4] |= 0x1;
        display[5] |= 0x1;
      } else {
        // done!
        displaymode = SHOW_TIME;
        return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;

      if (mode == SET_HOUR) {
        hour = (hour+1) % 24;
        display_alarm(hour, min);
        display[1] |= 0x1;
        display[2] |= 0x1;
        end_hour = hour;
        end_min = min;
        eeprom_write_byte((uint8_t *)EE_CD_HOUR, end_hour);    
        eeprom_write_byte((uint8_t *)EE_CD_MIN, end_min);    
        countdown_init(); //recalculate countdown
      }
      if (mode == SET_MIN) {
        min = (min+1) % 60;
        display_alarm(hour, min);
        display[4] |= 0x1;
        display[5] |= 0x1;
        end_hour = hour;
        end_min = min;
        eeprom_write_byte((uint8_t *)EE_CD_HOUR, end_hour);    
        eeprom_write_byte((uint8_t *)EE_CD_MIN, end_min);    
        countdown_init(); //recalculate countdown
      }

      if (pressed & 0x4)
        delayms(75);
    }
  }
}


void set_brightness(void) {
  uint8_t mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;;  

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      eeprom_write_byte((uint8_t *)EE_BRIGHT, brightness_level);
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {

      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // start!
        mode = SET_BRITE;
        // display brightness
        display_str("brite ");
        display_brightness(brightness_level);
      } else {	
        displaymode = SHOW_TIME;
        eeprom_write_byte((uint8_t *)EE_BRIGHT, brightness_level);
        return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      if (mode == SET_BRITE) {
        // Increment brightness level. Zero means auto-dim.
        if (brightness_level == 0) {
          brightness_level = BRIGHTNESS_MIN;
        } else {
          brightness_level += BRIGHTNESS_INCREMENT;
          if (brightness_level > BRIGHTNESS_MAX) {
#ifdef FEATURE_AUTODIM
            brightness_level = 0;
#else
            brightness_level = BRIGHTNESS_MIN;
#endif
          }
        }
        display_brightness(brightness_level);
      }
    }
  }
}

void display_brightness(int brightness) {
#ifdef FEATURE_AUTODIM
  if (brightness == 0) {
    // auto-dim
    display[7] =  pgm_read_byte(alphatable_p + 'a' - 'a') | 0x1;
    display[8] =  pgm_read_byte(alphatable_p + 'u' - 'a') | 0x1;
    dimmer_update();
    return;
  }
#endif
  display[7] = pgm_read_byte(numbertable_p + (brightness / 10)) | 0x1;
  display[8] = pgm_read_byte(numbertable_p + (brightness % 10)) | 0x1;
  set_vfd_brightness(brightness);
}

#ifdef FEATURE_DST
void set_dst(void) {
  uint8_t mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;;  

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // start!
        mode = SET_DST;
  switch(dst_on)
  {
    #ifdef DST_USA
    case (DST_USA):
      display_str("dst us  ");
      break;
    #endif
    #ifdef DST_GERMANY
    case (DST_GERMANY):
      display_str("dst de  ");
      break;
    #endif
    default:
      display_str("dst off ");
  }
      } else {	
        displaymode = SHOW_TIME;
        return;
      }
    }
    if (just_pressed & 0x4) {
      just_pressed = 0;
      if (mode == SET_DST) {
          dst_set = 0;
    switch(dst_on)
    {
      case (DST_OFF):
      default:
       #ifdef DST_USA
        dst_on = DST_USA;
        display_str("dst us  ");
        break;
      case (DST_USA):
       #endif
       #ifdef DST_GERMANY
        dst_on = DST_GERMANY;
        display_str("dst de  ");
        break;
      case (DST_GERMANY):
       #endif
        dst_on = DST_OFF;
        display_str("dst off ");
    }
        eeprom_write_byte((uint8_t *)EE_DST, dst_on);
      }
    }
  }
}
#endif

#ifdef FEATURE_TESTMODE
void set_test(void) {
  uint8_t mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;;  

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // start!
              testmode(1);
        displaymode = SHOW_TIME;
        return;
      }
    }
    if (just_pressed & 0x4) {
      just_pressed = 0;
    }
  }
}
#endif

void set_volume(void) {
  uint8_t mode = SHOW_MENU;
  uint8_t volume;

  timeoutcounter = INACTIVITYTIMEOUT;;  
  volume = eeprom_read_byte((uint8_t *)EE_VOLUME);

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // start!
        mode = SET_VOL;
        // display volume
        if (volume) {
          display_str("vol high");
          display[5] |= 0x1;
        } else {
          display_str("vol  low");
        }
        display[6] |= 0x1;
        display[7] |= 0x1;
        display[8] |= 0x1;
      } else {	
        displaymode = SHOW_TIME;
        return;
      }
    }
    if (just_pressed & 0x4) {
      just_pressed = 0;
      if (mode == SET_VOL) {
        volume = !volume;
        if (volume) {
          display_str("vol high");
          display[5] |= 0x1;
        } else {
          display_str("vol  low");
        }
        display[6] |= 0x1;
        display[7] |= 0x1;
        display[8] |= 0x1;
        eeprom_write_byte((uint8_t *)EE_VOLUME, volume);
        speaker_init();
        beep(4000, 1);
      }
    }
  }
}




void set_region(void) {
  uint8_t mode = SHOW_MENU;

  timeoutcounter = INACTIVITYTIMEOUT;;  
  region = eeprom_read_byte((uint8_t *)EE_REGION);

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {
      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // start!
        mode = SET_REG;
        // display region
        if (region == REGION_US) {
          display_str("usa-12hr");
        } else {
          display_str("eur-24hr");
        }
      } else {	
        displaymode = SHOW_TIME;
        return;
      }
    }
    if (just_pressed & 0x4) {
      just_pressed = 0;
      if (mode == SET_REG) {
        region = !region;
        if (region == REGION_US) {
          display_str("usa-12hr");
        } else {
          display_str("eur-24hr");
        }
        eeprom_write_byte((uint8_t *)EE_REGION, region);
      }
    }
  }
}


#ifdef FEATURE_SETSNOOZETIME
void set_snooze(void) {
  uint8_t mode = SHOW_MENU;
  uint8_t snooze;

  timeoutcounter = INACTIVITYTIMEOUT;;  
  snooze = eeprom_read_byte((uint8_t *)EE_SNOOZE);

  while (1) {
    if (just_pressed || pressed) {
      timeoutcounter = INACTIVITYTIMEOUT;;  
      // timeout w/no buttons pressed after 3 seconds?
    } else if (!timeoutcounter) {
      //timed out!
      displaymode = SHOW_TIME;     
      return;
    }
    if (just_pressed & 0x1) { // mode change
      return;
    }
    if (just_pressed & 0x2) {

      just_pressed = 0;
      if (mode == SHOW_MENU) {
        // start!
        mode = SET_SNOOZE;
        // display snooze
        display_str("   minut");
        display[1] = pgm_read_byte(numbertable_p + (snooze / 10)) | 0x1;
        display[2] = pgm_read_byte(numbertable_p + (snooze % 10)) | 0x1;
      } else { 
        displaymode = SHOW_TIME;
        return;
      }
    }
    if ((just_pressed & 0x4) || (pressed & 0x4)) {
      just_pressed = 0;
      if (mode == SET_SNOOZE) {
        snooze ++;
        if (snooze >= 100)
          snooze = 0;
        display[1] = pgm_read_byte(numbertable_p + (snooze / 10)) | 0x1;
        display[2] = pgm_read_byte(numbertable_p + (snooze % 10)) | 0x1;
        eeprom_write_byte((uint8_t *)EE_SNOOZE, snooze);
      }

      if (pressed & 0x4)
        delayms(75);

    }
  }
}
#endif


/**************************** RTC & ALARM *****************************/
void clock_init(void) {
  // we store the time in EEPROM when switching from power modes so its
  // reasonable to start with whats in memory
  time_h = eeprom_read_byte((uint8_t *)EE_HOUR) % 24;
  time_m = eeprom_read_byte((uint8_t *)EE_MIN) % 60;
  time_s = eeprom_read_byte((uint8_t *)EE_SEC) % 60;

  /*
    // if you're debugging, having the makefile set the right
    // time automatically will be very handy. Otherwise don't use this
  time_h = TIMEHOUR;
  time_m = TIMEMIN;
  time_s = TIMESEC + 10;
  */

  // Set up the stored alarm time and date
  alarm_m = eeprom_read_byte((uint8_t *)EE_ALARM_MIN) % 60;
  alarm_h = eeprom_read_byte((uint8_t *)EE_ALARM_HOUR) % 24;

  date_y = eeprom_read_byte((uint8_t *)EE_YEAR) % 100;
  date_m = eeprom_read_byte((uint8_t *)EE_MONTH) % 13;
  date_d = eeprom_read_byte((uint8_t *)EE_DAY) % 32;

  restored = 1;

  // Turn on the RTC by selecting the external 32khz crystal
  // 32.768 / 128 = 256 which is exactly an 8-bit timer overflow
  ASSR |= _BV(AS2); // use crystal
  TCCR2B = _BV(CS22) | _BV(CS20); // div by 128
  // We will overflow once a second, and call an interrupt

  // enable interrupt
  TIMSK2 = _BV(TOIE2);

  // enable all interrupts!
  sei();
  countdown_init();
}

// Calculate or refresh time interval for countdown
void countdown_init(void) {
  end_year = eeprom_read_byte((uint8_t *)EE_CD_YEAR) % 100;
  end_month = eeprom_read_byte((uint8_t *)EE_CD_MONTH) % 13;
  end_day = eeprom_read_byte((uint8_t *)EE_CD_DAY) % 32;
  end_hour = eeprom_read_byte((uint8_t *)EE_CD_HOUR) % 24;
  end_min = eeprom_read_byte((uint8_t *)EE_CD_MIN) % 60;
  cd_d  = dayofyear(end_year, end_month, end_day);
  cd_d -= dayofyear(date_y, date_m, date_d);
  cd_h  = end_hour - time_h;
  cd_m  = end_min - time_m;
  cd_s  = -time_s;
  
  uint8_t y = end_year;
  while(y > date_y){
    cd_d += 365;
    if (leapyear(2000+y-1)) cd_d++;
    y--;
  } 
  while(y < date_y){
    cd_d -= 365;
    if (leapyear(2000+y-1)) cd_d--;
    y++;
  } 
  
  if (cd_s<0) {
    cd_s += 60;
    cd_m--;
  }
  if (cd_m<0) {
    cd_m += 60;
    cd_h--;
  }
  if (cd_h<0) {
    cd_h += 24;
    cd_d--;
  }
}

// day number in year
int16_t dayofyear(uint8_t y, uint8_t m, uint8_t d){
  uint16_t day = d;
  switch(m) {
  case 12:
    day += 30; //add 30 days in Nov.
  case 11:
    day += 31; //add 31 days in Oct.
  case 10:
    day += 30; //add 30 days in Sep.
  case 9:
    day += 31; //add 31 days in Aug.
  case 8:
    day += 31; //add 31 days in Jul.
  case 7:
    day += 30; //add 30 days in Jun.
  case 6:
    day += 31; //add 31 days in May.
  case 5:
    day += 30; //add 30 days in Apr.
  case 4:
    day += 31; //add 31 days in Mar.
  case 3:
    day += 28; //add 28 days in Feb.
    if (leapyear(2000+y)) day++; //plus one if needed for leapyear
  case 2:
    day += 31; //add 31 days in Jan.
  case 1:
  default:
    return day;
  }
}

// This turns on/off the alarm when the switch has been
// set. It also displays the alarm time
void setalarmstate(void) {
  if (ALARM_PIN & _BV(ALARM)) { 
    // Don't display the alarm/beep if we already have
    if  (!alarm_on) {
      // alarm on!
      alarm_on = 1;
      // reset snoozing
      snoozetimer = 0;
      // its not actually SHOW_SNOOZE but just anything but SHOW_TIME
      if(displaymode == SHOW_TIME) //If we are in test mode, we would mess up
      {                           //testing of the display segments.
        // show the status on the VFD tube
        display_str("alarm on");
        displaymode = SHOW_SNOOZE;
        delayms(1000);
        // show the current alarm time set
        display_alarm(alarm_h, alarm_m);
        delayms(1000);
        // after a second, go back to clock mode
        displaymode = SHOW_TIME;
      }
    }
  } else {
    if (alarm_on) {
      // turn off the alarm
      alarm_on = 0;
      snoozetimer = 0;
      if (alarming) {
        // if the alarm is going off, we should turn it off
        // and quiet the speaker
        DEBUGP("alarm off");
        alarming = 0;
        TCCR1B &= ~_BV(CS11); // turn it off!
        PORTB |= _BV(SPK1) | _BV(SPK2);
      } 
    }
  }
}

// This will calculate leapyears, give it the year
// and it will return 1 (true) or 0 (false)
uint8_t leapyear(uint16_t y) {
  return ( (!(y % 4) && (y % 100)) || !(y % 400));
}


/**************************** SPEAKER *****************************/
// Set up the speaker to prepare for beeping!
void speaker_init(void) {

  // read the preferences for high/low volume
  volume = eeprom_read_byte((uint8_t *)EE_VOLUME);

  // We use the built-in fast PWM, 8 bit timer
  PORTB |= _BV(SPK1) | _BV(SPK2); 

  // Turn on PWM outputs for both pins
  TCCR1A = _BV(COM1B1) | _BV(COM1B0) | _BV(WGM11);
  if (volume) {
    TCCR1A |= _BV(COM1A1);
  } 
  TCCR1B = _BV(WGM13) | _BV(WGM12);

  // start at 4khz:  250 * 8 multiplier * 4000 = 8mhz
  ICR1 = 250;
  OCR1B = OCR1A = ICR1 / 2;
}

// This makes the speaker tick, it doesnt use PWM
// instead it just flicks the piezo
void tick(void) {
  TCCR1A = 0;
  TCCR1B = 0;

  // Send a pulse thru both pins, alternating
  SPK_PORT |= _BV(SPK1);
  SPK_PORT &= ~_BV(SPK2);
  delayms(10);
  SPK_PORT |= _BV(SPK2);
  SPK_PORT &= ~_BV(SPK1);
  delayms(10);
  // turn them both off
  SPK_PORT &= ~_BV(SPK1) & ~_BV(SPK2);

  TCCR1A = _BV(COM1A1) | _BV(COM1B1) | _BV(COM1B0) | _BV(WGM11);
  TCCR1B = _BV(WGM13) | _BV(WGM12);
}

// We can play short beeps!
void beep(uint16_t freq, uint8_t times) {
  // set the PWM output to match the desired frequency
  ICR1 = (F_CPU/8)/freq;
  // we want 50% duty cycle square wave
  OCR1A = OCR1B = ICR1/2;
   
  while (times--) {
    TCCR1B |= _BV(CS11); // turn it on!
    // beeps are 200ms long on
    _delay_ms(200);
    TCCR1B &= ~_BV(CS11); // turn it off!
    PORTB &= ~_BV(SPK1) & ~_BV(SPK2);
    // beeps are 200ms long off
    _delay_ms(200);
  }
  // turn speaker off
  PORTB &= ~_BV(SPK1) & ~_BV(SPK2);
}


#ifdef FEATURE_AUTODIM
/**************************** DIMMER ****************************/
void dimmer_init(void) {
  // Power for the photoresistor
  DIMMER_POWER_DDR |= _BV(DIMMER_POWER_PIN); 
  DIMMER_POWER_PORT |= _BV(DIMMER_POWER_PIN);

  ADCSRA |= _BV(ADPS2)| _BV(ADPS1); // Set ADC prescalar to 64 - 125KHz sample rate @ 8MHz F_CPU
  ADMUX |= _BV(REFS0);  // Set ADC reference to AVCC
  ADMUX |= _BV(DIMMER_SENSE_PIN);   // Set ADC input as ADC4 (PC4)
  DIDR0 |= _BV(DIMMER_SENSE_PIND); // Disable the digital imput buffer on the sense pin to save power.
  ADCSRA |= _BV(ADEN);  // Enable ADC
  ADCSRA |= _BV(ADIE);  // Enable ADC interrupt
}

// Start ADC conversion for dimmer
void dimmer_update(void) {
  if (brightness_level == 0) 
    ADCSRA |= _BV(ADSC);
}

// Update brightness once ADC measurement completes
SIGNAL(SIG_ADC) {
  uint8_t low, high;
  unsigned int val;
  if (brightness_level != 0)
    return;
  // Read 2-byte value. Must read ADCL first because that locks the value.
  low = ADCL;
  high = ADCH;
  val = (high << 8) | low;
  #ifdef FEATURE_TESTMODE
  dimmer_level = val;
  #endif
  // Set brightness to a value between min & max based on light reading.
  if (val >= PHOTOCELL_DARK) {
    val = BRIGHTNESS_MIN;
  } else if (val <= PHOTOCELL_LIGHT) {
    val = BRIGHTNESS_MAX;
  } else {
    val = BRIGHTNESS_MAX - (((unsigned long)(BRIGHTNESS_MAX - BRIGHTNESS_MIN)) *
        (val - PHOTOCELL_LIGHT)) / (PHOTOCELL_DARK - PHOTOCELL_LIGHT);
  }
  set_vfd_brightness(val);
}
#endif

/**************************** BOOST *****************************/

// We control the boost converter by changing the PWM output
// pins
void boost_init(uint8_t brightness) {

  set_vfd_brightness(brightness);

  // fast PWM, set OC0A (boost output pin) on match
  TCCR0A = _BV(WGM00) | _BV(WGM01);  

  // Use the fastest clock
  TCCR0B = _BV(CS00);
 
  TCCR0A |= _BV(COM0A1);
  TIMSK0 |= _BV(TOIE0); // turn on the interrupt for muxing
  sei();
}

void set_vfd_brightness(uint8_t brightness) {
  // Set PWM value, don't set it so high that
  // we could damage the MAX chip or display
  if (brightness > BRIGHTNESS_MAX)
    brightness = BRIGHTNESS_MAX;

  // Or so low its not visible
  if (brightness < BRIGHTNESS_MIN)
    brightness = BRIGHTNESS_MIN;

  // Round up to the next brightness increment
  //if (brightness % BRIGHTNESS_INCREMENT != 0) {
  //  brightness += BRIGHTNESS_INCREMENT - (brightness % BRIGHTNESS_INCREMENT);
  //}

  if (OCR0A == brightness)
    return;

  OCR0A = brightness;
}

uint8_t dotw(void)
{
  uint16_t month, year;

    // Calculate day of the week
    
    month = date_m;
    year = 2000 + date_y;
    if (date_m < 3)  {
      month += 12;
      year -= 1;
    }
    return (date_d + (2 * month) + (6 * (month+1)/10) + year + (year/4) - (year/100) + (year/400) + 1) % 7;
}

/**************************** DISPLAY *****************************/

// We can display the current date!
void display_date(uint8_t style) {

  // This type is mm-dd-yy OR dd-mm-yy depending on our pref.
  if (style == DATE) {
    display[0] = 0;
    display[6] = display[3] = 0x02;     // put dashes between num

    if (region == REGION_US) {
      // mm-dd-yy
      display[1] = pgm_read_byte(numbertable_p + (date_m / 10));
      display[2] = pgm_read_byte(numbertable_p + (date_m % 10));
      display[4] = pgm_read_byte(numbertable_p + (date_d / 10));
      display[5] = pgm_read_byte(numbertable_p + (date_d % 10));
    } else {
      // dd-mm-yy
      display[1] = pgm_read_byte(numbertable_p + (date_d / 10));
      display[2] = pgm_read_byte(numbertable_p + (date_d % 10));
      display[4] = pgm_read_byte(numbertable_p + (date_m / 10));
      display[5] = pgm_read_byte(numbertable_p + (date_m % 10));
    }
    // the yy part is the same
    display[7] = pgm_read_byte(numbertable_p + (date_y / 10));
    display[8] = pgm_read_byte(numbertable_p + (date_y % 10));

  } else if (style == DAY) {
    // This is more "Sunday June 21" style

    

    // Display the day first
    display[8] = display[7] = 0;
    switch (dotw()) {
    case 0:
      display_str("sunday"); break;
    case 1:
      display_str("monday"); break;
    case 2:
      display_str("tuesday"); break;
    case 3:
      display_str("wednsday"); break;
    case 4:
      display_str("thursday"); break;
    case 5:
      display_str("friday"); break;
    case 6:
      display_str("saturday"); break;
    }
    
    // wait one seconds about
    delayms(1000);

    // Then display the month and date
    display[6] = display[5] = display[4] = 0;
    switch (date_m) {
    case 1:
      display_str("jan"); break;
    case 2:
      display_str("feb"); break;
    case 3:
      display_str("march"); break;
    case 4:
      display_str("april"); break;
    case 5:
      display_str("may"); break;
    case 6:
      display_str("june"); break;
    case 7:
      display_str("july"); break;
    case 8:
      display_str("augst"); break;
    case 9:
      display_str("sept"); break;
    case 10:
      display_str("octob"); break;
    case 11:
      display_str("novem"); break;
    case 12:
      display_str("decem"); break;
    }
    display[7] = pgm_read_byte(numbertable_p + (date_d / 10));
    display[8] = pgm_read_byte(numbertable_p + (date_d % 10));
    
  }
}

// This displays a time on the clock
void display_time(uint8_t h, uint8_t m, uint8_t s) {
  
  // seconds and minutes are at the end
  display[8] =  pgm_read_byte(numbertable_p + (s % 10));
  display[7] =  pgm_read_byte(numbertable_p + (s / 10));
  display[6] = 0;
  display[5] =  pgm_read_byte(numbertable_p + (m % 10));
  display[4] =  pgm_read_byte(numbertable_p + (m / 10)); 
  display[3] = 0;

  // check euro (24h) or US (12h) style time
  if (region == REGION_US) {
    display[2] =  pgm_read_byte(numbertable_p + ( (((h+11)%12)+1) % 10));
    if ((((h+11)%12)+1) / 10 == 0 ) {
      display[1] =  0;
    } else {
      display[1] =  pgm_read_byte(numbertable_p + 1);
    }

    // We use the '*' as an am/pm notice
    if (h >= 12)
      display[0] |= 0x1;  // 'pm' notice
    else 
      display[0] &= ~0x1;  // 'pm' notice
  } else {
    display[2] =  pgm_read_byte(numbertable_p + ( (h%24) % 10));
    display[1] =  pgm_read_byte(numbertable_p + ( (h%24) / 10));
  }
}

// Kinda like display_time but just hours and minutes
void display_alarm(uint8_t h, uint8_t m){ 
  display[8] = 0;
  display[7] = 0;
  display[6] = 0;
  display[5] = pgm_read_byte(numbertable_p + (m % 10));
  display[4] = pgm_read_byte(numbertable_p + (m / 10)); 
  display[3] = 0;
  

  // check euro or US style time
  if (region == REGION_US) {
    if (h >= 12) {
      display[0] |= 0x1;  // 'pm' notice
      display[7] = pgm_read_byte(alphatable_p + 'p' - 'a');
    } else {
      display[7] = pgm_read_byte(alphatable_p + 'a' - 'a');
      display[0] &= ~0x1;  // 'am' notice
    }
    display[8] = pgm_read_byte(alphatable_p + 'm' - 'a');

    display[2] =  pgm_read_byte(numbertable_p + ( (((h+11)%12)+1) % 10));
    if ((((h+11)%12)+1) / 10 == 0 ) {
      display[1] =  0;
    } else {
      display[1] =  pgm_read_byte(numbertable_p + 1);
    }
  } else {
    display[2] =  pgm_read_byte(numbertable_p + ( (((h+23)%24)+1) % 10));
    display[1] =  pgm_read_byte(numbertable_p + ( (((h+23)%24)+1) / 10));
  }
}

// display words (menus, prompts, etc)
void display_str(char *s) {
  uint8_t i;

  // don't use the lefthand dot/slash digit
  display[0] = 0;

  // up to 8 characters
  for (i=1; i<9; i++) {
    // check for null-termination
    if (s[i-1] == 0)
      return;

    // Numbers and leters are looked up in the font table!
    if ((s[i-1] >= 'a') && (s[i-1] <= 'z')) {
      display[i] =  pgm_read_byte(alphatable_p + s[i-1] - 'a');
    } else if ((s[i-1] >= '0') && (s[i-1] <= '9')) {
      display[i] =  pgm_read_byte(numbertable_p + s[i-1] - '0');
    } else {
      display[i] = 0;      // spaces and other stuff are ignored :(
    }
  }
}

// display countdown
void display_countdown(int16_t d, int8_t h, int8_t m, int8_t s){  
  if (d<0){
    d = -1 - d;
    h = 23 - h;
    m = 59 - m;
    s = 59 - s;
  }
  if (d<100){
    // seconds and minutes are at the end
    display[8] = pgm_read_byte(numbertable_p + (s % 10));
    display[7] = pgm_read_byte(numbertable_p + (s / 10));
    display[6] = pgm_read_byte(numbertable_p + (m % 10)) | 0x01;
    display[5] = pgm_read_byte(numbertable_p + (m / 10)); 
    display[4] = pgm_read_byte(numbertable_p + (h % 10)) | 0x01;
    display[3] = pgm_read_byte(numbertable_p + (h / 10)); 
    display[2] = pgm_read_byte(numbertable_p + (d % 10)) | 0x01;
    display[1] = pgm_read_byte(numbertable_p + (d / 10)); 
    display[0] = 0;
  } else {
    display[8] = pgm_read_byte(numbertable_p + (m % 10));
    display[7] = pgm_read_byte(numbertable_p + (m / 10)); 
    display[6] = pgm_read_byte(numbertable_p + (h % 10)) | 0x01;
    display[5] = pgm_read_byte(numbertable_p + (h / 10)); 
    display[4] = pgm_read_byte(numbertable_p + (d % 10)) | 0x01;
    display[3] = pgm_read_byte(numbertable_p + ((d / 10) % 10)); 
    display[2] = pgm_read_byte(numbertable_p + ((d / 100) % 10)); 
    display[1] = pgm_read_byte(numbertable_p + ((d / 1000) % 10));   
    display[0] = 0;
  }
}

// We can display the end of countdown
void display_cd_date(void) {

  // This type is mm-dd-yy OR dd-mm-yy depending on our pref.
  display[0] = 0;
  display[6] = display[3] = 0x02;     // put dashes between num

  if (region == REGION_US) {
    // mm-dd-yy
    display[1] = pgm_read_byte(numbertable_p + (end_month / 10));
    display[2] = pgm_read_byte(numbertable_p + (end_month % 10));
    display[4] = pgm_read_byte(numbertable_p + (end_day / 10));
    display[5] = pgm_read_byte(numbertable_p + (end_day % 10));
  } else {
    // dd-mm-yy
    display[1] = pgm_read_byte(numbertable_p + (end_day / 10));
    display[2] = pgm_read_byte(numbertable_p + (end_day % 10));
    display[4] = pgm_read_byte(numbertable_p + (end_month / 10));
    display[5] = pgm_read_byte(numbertable_p + (end_month % 10));
  }
  // the yy part is the same
  display[7] = pgm_read_byte(numbertable_p + (end_year / 10));
  display[8] = pgm_read_byte(numbertable_p + (end_year % 10));
}

/************************* LOW LEVEL DISPLAY ************************/

// Setup SPI
void vfd_init(void) {
  SPCR  = _BV(SPE) | _BV(MSTR) | _BV(SPR0);
}

// This changes and updates the display
// We use the digit/segment table to determine which
// pins on the MAX6921 to turn on
void setdisplay(uint8_t digit, uint8_t segments) {
  uint32_t d = 0;  // we only need 20 bits but 32 will do
  uint8_t i;

  // Set the digit selection pin
  d |= _BV(pgm_read_byte(digittable_p + digit));

  
  // Set the individual segments for this digit
  for (i=0; i<8; i++) {
    if (segments & _BV(i)) {
      t = 1;
      t <<= pgm_read_byte(segmenttable_p + i);
      d |= t;
    }
  }

  // Shift the data out to the display
  vfd_send(d);
}

// send raw data to display, its pretty straightforward. Just send 32 bits via SPI
// the bottom 20 define the segments
void vfd_send(uint32_t d) {
  // send lowest 20 bits
  cli();       // to prevent flicker we turn off interrupts
  spi_xfer(d >> 16);
  spi_xfer(d >> 8);
  spi_xfer(d);

  // latch data
  VFDLOAD_PORT |= _BV(VFDLOAD);
  VFDLOAD_PORT &= ~_BV(VFDLOAD);
  sei();
}

// Send 1 byte via SPI
void spi_xfer(uint8_t c) {

  SPDR = c;
  while (! (SPSR & _BV(SPIF)));
}

