/*
 * MCU_project.c
 *
 * Keypad clock (MM:SS) with alarm and stopwatch for the ATmega128 board.
 *
 * Keys:
 *   0-9  Digits (only used while setting the clock or the alarm)
 *   A    Finish: apply the time being entered
 *   B    Abandon: discard the entry, leave the current mode, return to the clock
 *   C    Arm or disarm the alarm
 *   D    Set the clock
 *   E    Set the alarm
 *   F    Stopwatch: 1st press starts from 00:00, 2nd press stops, 3rd press returns to the clock
 *   Any key silences a ringing alarm.
 */

#include "c4mlib.h"
#include <avr/interrupt.h>
#include <util/atomic.h>

/*---------- Constants ----------*/

/* Key types returned by KeyResolver() */
#define number 1
#define sign   0

/* Function keys */
#define KEY_FINISH     'A'
#define KEY_ABANDON    'B'
#define KEY_ALARM      'C'
#define KEY_SETCLOCK   'D'
#define KEY_SETALARM   'E'
#define KEY_STOPWATCH  'F'

/* System modes */
#define MODE_CLOCK      0       /* Show the current time */
#define MODE_SET_CLOCK  1       /* Typing a new clock time (steady display) */
#define MODE_SET_ALARM  2       /* Typing a new alarm time (flashing display) */
#define MODE_STOPWATCH  3       /* Show the stopwatch */

/* Seven-segment codes: bit 6 = a ... bit 0 = g, 1 = segment on */
#define SEG_BLANK 0x00
#define SEG_DASH  0x01

/* Timer1 ticks per second (100 Hz compare-match interrupt) */
#define TICKS_PER_SEC 100

/*---------- Function Declarations ----------*/
static void BusWrite(uint8_t addr, uint8_t data);
static void SelectRow(uint8_t pattern);
static uint8_t ReadColumn(uint8_t col);
static char KeyBoard(void);
static uint8_t KeyResolver(char key, uint8_t *digit);
static void KeyInCumulator(uint8_t digit);
static void Calculator(char key);
static void display(int value);
static void FlashDark(void);
static void InitializeLED(void);
static void UpdateDisplay(void);

/*---------- Variables shared with interrupts ----------*/
volatile uint8_t hsc = 0;                       /* Hundredths of a second (0-99) */
volatile uint8_t min = 0, sec = 0;              /* Clock time */
volatile uint8_t swTicks = 0;                   /* Stopwatch hundredths */
volatile uint8_t swMin = 0, swSec = 0;          /* Stopwatch time */
volatile uint8_t swRunning = 0;                 /* 1 while the stopwatch counts */
volatile uint8_t alarmMin = 0, alarmSec = 0;    /* Alarm time */
volatile uint8_t alarmArmed = 0;                /* 1 when the alarm is on */
volatile uint8_t alarmRinging = 0;              /* Set by the timer ISR when the alarm time is reached */
volatile uint8_t secondTick = 0;                /* Set by the timer ISR once per second */
volatile uint8_t keyRequest = 0;                /* Set by the INT5 ISR when a key goes down */

/*---------- Variables used only by the main loop ----------*/
static uint8_t mode = MODE_CLOCK;               /* Current system mode */
static int entry = 0;                           /* Digits typed so far, as MMSS */
static uint8_t entryError = 0;                  /* 1 after an invalid entry; shows "----" */

static const uint8_t SevenSegTable[10] = {126, 48, 109, 121, 51, 91, 95, 112, 127, 123};


int main(void)
{
	C4M_DEVICE_set();

	REGFPT(&DDRD, 0x70, 0, 0x70);           // Set PD4-PD6 as outputs (138 decoder select)
	REGFPT(&DDRE, 0x18, 0, 0x18);           // Set PE3-PE4 as outputs (read/write strobes)

	InitializeLED();                        // Show 0000
	SelectRow(0x00);                        // Drive every keypad line low so any key can trigger INT5

	/* Timer1: CTC mode, prescaler 64, 100 Hz interrupt at 11.0592 MHz */
	REGFPT(&TCCR1A, 0x03, 0, 0);            /* Clear WGM11:10 */
	REGFPT(&TCCR1B, 0x18, 3, 1);            /* Set WGM13:12 to 01 (CTC mode, TOP = OCR1A) */
	REGFPT(&TCCR1B, 0x07, 0, 3);            /* Set clock prescaler to divide by 64 */
	OCR1A = 1727;                           // F_CLKIO / (64 * (1 + 1727)) = 100 Hz
	REGFPT(&TIMSK, 0x10, 4, 1);             /* Enable the Timer1 compare match A interrupt */

	/* INT5: keypad interrupt on a falling edge */
	REGFPT(&DDRE, 0x20, 5, 0);              /* Set PE5 (INT5 source pin) as input */
	REGFPT(&EICRB, 0x0C, 2, 2);             /* Set INT5 sense control bits to 10 (falling edge) */
	EIFR = (1 << INTF5);                    /* Clear any pending INT5 request */
	REGFPT(&EIMSK, 0x20, 5, 1);             /* Enable external interrupt INT5 */

	sei();                                  /* Enable global interrupts */

	while (1)
	{
		/* Scan the keypad here, not in the ISR, so the timer interrupt is never blocked */
		if (keyRequest)
		{
			char key = KeyBoard();

			/* The scan itself toggles the INT5 line; drop those requests */
			keyRequest = 0;
			EIFR = (1 << INTF5);

			if (key != 0)
			{
				uint8_t digit;
				printf("Key %c\n", key);

				if (alarmRinging)                       // Any key silences the alarm
				{
					alarmRinging = 0;
					printf("Alarm stopped\n");
				}
				else if (KeyResolver(key, &digit) == number)
				{
					KeyInCumulator(digit);
				}
				else
				{
					Calculator(key);
				}
			}
		}

		if (secondTick)                                 // Once per second: debug output
		{
			uint8_t m, s;
			secondTick = 0;
			ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { m = min; s = sec; }
			printf("Time %02u:%02u%s\n", m, s, alarmRinging ? "  ALARM" : "");
		}

		UpdateDisplay();
	}
}

/*---------- Interrupt service routines ----------*/

ISR(TIMER1_COMPA_vect)
{
	if (++hsc >= TICKS_PER_SEC)
	{
		hsc = 0;
		if (++sec > 59)
		{
			sec = 0;
			if (++min > 59)
			{
				min = 0;
			}
		}
		if (alarmArmed && (min == alarmMin) && (sec == alarmSec))
		{
			alarmRinging = 1;
		}
		secondTick = 1;
	}

	if (swRunning && (++swTicks >= TICKS_PER_SEC))
	{
		swTicks = 0;
		if (++swSec > 59)
		{
			swSec = 0;
			if (++swMin > 59)
			{
				swMin = 0;
			}
		}
	}
}

ISR(INT5_vect)
{
	keyRequest = 1;                         // Just flag it; the main loop does the scan
}

/*---------- Bus access ----------*/

/* Write one byte to the 374 latch selected by decoder output addr */
static void BusWrite(uint8_t addr, uint8_t data)
{
	uint8_t allOut = 0xFF;
	REGPUT(&DDRB, 1, &allOut);              // Whole data bus as outputs
	REGFPT(&PORTD, 0x70, 4, addr);          // Address on PD4-PD6
	REGFPT(&PORTE, 0x18, 3, 0x00);          // Both strobes low
	REGPUT(&PORTB, 1, &data);               // Data on PB0-PB7
	REGFPT(&PORTE, 0x18, 3, 0x02);          // Rising edge on PE4 latches the byte
}

/* Write a row drive pattern into the keypad latch (decoder output O0) */
static void SelectRow(uint8_t pattern)
{
	REGFPT(&DDRB, 0xFF, 0, 0x0F);           // PB0-PB3 outputs (row data), PB4-PB7 inputs (columns)
	REGFPT(&PORTD, 0x70, 4, 0x00);          // Select decoder output O0 (374 latch clock)
	REGFPT(&PORTE, 0x18, 3, 0x00);          // Both strobes low
	REGFPT(&PORTB, 0x0F, 0, pattern);       // Row pattern on PB0-PB3
	REGFPT(&PORTE, 0x18, 3, 0x02);          // Rising edge on PE4 latches the pattern
}

/* Read keypad column col (0-3, on PB4-PB7). Returns 0 when a key in that column is pressed. */
static uint8_t ReadColumn(uint8_t col)
{
	uint8_t in;
	REGFPT(&PORTE, 0x08, 3, 0x01);          // Raise PE3 (read enable)
	_delay_ms(20);                          // Settle and debounce
	REGFGT(&PINB, (uint8_t)(0x10 << col), 4 + col, &in);
	REGFPT(&PORTE, 0x08, 3, 0x00);          // Drop PE3
	return in;
}

/*---------- Keypad ----------*/

/* Scan the keypad. Waits for the key to be released and returns it, or 0 if none is pressed. */
static char KeyBoard(void)
{
	const uint8_t RowPins[4] = {14, 13, 11, 7};    // Drive one line low at a time: 1110, 1101, 1011, 0111
	const char keymap[4][4] = {                     // Key labels by drive line and column
		{'7','4','1','0'},
		{'8','5','2','A'},
		{'9','6','3','B'},
		{'C','D','E','F'}
	};
	uint8_t n, c;
	char key = 0;

	for (n = 0; (n < 4) && (key == 0); n++)
	{
		SelectRow(RowPins[n]);
		for (c = 0; c < 4; c++)
		{
			if (ReadColumn(c) == 0)
			{
				while (ReadColumn(c) == 0) { }  // Wait for the key to be released
				key = keymap[n][c];
				break;
			}
		}
	}

	SelectRow(0x00);                        // Restore the idle state: every line low
	return key;
}

/* Classify a key. Digits return number and store the value in *digit; others return sign. */
static uint8_t KeyResolver(char key, uint8_t *digit)
{
	if ((key >= '0') && (key <= '9'))
	{
		*digit = (uint8_t)(key - '0');
		return number;
	}
	return sign;
}

/* Add a digit to the entry. Keeps the last four digits typed, so "1230" becomes 12:30. */
static void KeyInCumulator(uint8_t digit)
{
	if ((mode != MODE_SET_CLOCK) && (mode != MODE_SET_ALARM))
	{
		return;                             // Digits only matter while setting a time
	}
	entry = (entry % 1000) * 10 + digit;
	entryError = 0;
}

/* Act on a function key */
static void Calculator(char key)
{
	switch (key)
	{
	case KEY_SETCLOCK:
		mode = MODE_SET_CLOCK;
		entry = 0;
		entryError = 0;
		break;

	case KEY_SETALARM:
		mode = MODE_SET_ALARM;
		entry = 0;
		entryError = 0;
		break;

	case KEY_FINISH:
		if ((mode == MODE_SET_CLOCK) || (mode == MODE_SET_ALARM))
		{
			uint8_t m = (uint8_t)(entry / 100);
			uint8_t s = (uint8_t)(entry % 100);

			if ((m > 59) || (s > 59))       // Invalid time: show "----" and stay in this mode
			{
				entry = 0;
				entryError = 1;
				printf("Invalid time\n");
				break;
			}

			if (mode == MODE_SET_CLOCK)
			{
				ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { min = m; sec = s; hsc = 0; }
				printf("Clock set to %02u:%02u\n", m, s);
			}
			else
			{
				ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { alarmMin = m; alarmSec = s; }
				printf("Alarm set to %02u:%02u\n", m, s);
			}
			mode = MODE_CLOCK;
		}
		break;

	case KEY_ABANDON:
		entry = 0;
		entryError = 0;
		swRunning = 0;
		mode = MODE_CLOCK;
		printf("Abandoned\n");
		break;

	case KEY_ALARM:
		alarmArmed = !alarmArmed;
		printf("Alarm %s\n", alarmArmed ? "on" : "off");
		break;

	case KEY_STOPWATCH:
		if (mode != MODE_STOPWATCH)             // 1st press: start from 00:00
		{
			ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { swTicks = 0; swSec = 0; swMin = 0; swRunning = 1; }
			mode = MODE_STOPWATCH;
		}
		else if (swRunning)                     // 2nd press: stop and hold the time
		{
			swRunning = 0;
		}
		else                                    // 3rd press: back to the clock
		{
			mode = MODE_CLOCK;
		}
		break;

	default:
		break;
	}
}

/*---------- Display ----------*/

/* Decide what the display should show and rewrite it only when that changes */
static void UpdateDisplay(void)
{
	static int lastValue = -2;
	static uint8_t lastBlank = 2;
	uint8_t m, s, h, sm, ss, ringing;
	uint8_t blank = 0;
	uint8_t blinkOn;
	int value;

	ATOMIC_BLOCK(ATOMIC_RESTORESTATE)
	{
		m = min; s = sec; h = hsc;
		sm = swMin; ss = swSec;
		ringing = alarmRinging;
	}
	blinkOn = (h < TICKS_PER_SEC / 2);      // On for the first half of each second

	switch (mode)
	{
	case MODE_SET_CLOCK:                    // Entered clock value, steady
		value = entryError ? -1 : entry;
		break;
	case MODE_SET_ALARM:                    // Entered alarm value, flashing
		value = entryError ? -1 : entry;
		blank = !blinkOn;
		break;
	case MODE_STOPWATCH:
		value = sm * 100 + ss;
		break;
	default:                                // Current time; flashing while the alarm rings
		value = m * 100 + s;
		blank = ringing && !blinkOn;
		break;
	}

	if ((value != lastValue) || (blank != lastBlank))
	{
		if (blank)
		{
			FlashDark();
		}
		else
		{
			display(value);
		}
		REGFPT(&DDRB, 0xFF, 0, 0x0F);       // Return the bus to its idle (scan-ready) state
		lastValue = value;
		lastBlank = blank;
	}
}

/* Show value (0-9999) as four digits; anything else shows "----" */
static void display(int value)
{
	uint8_t i;

	if ((value < 0) || (value > 9999))
	{
		for (i = 1; i <= 4; i++)
		{
			BusWrite(i, SEG_DASH);
		}
		return;
	}

	BusWrite(1, SevenSegTable[value / 1000]);
	BusWrite(2, SevenSegTable[(value / 100) % 10]);
	BusWrite(3, SevenSegTable[(value / 10) % 10]);
	BusWrite(4, SevenSegTable[value % 10]);
}

/* Turn every digit off */
static void FlashDark(void)
{
	uint8_t i;
	for (i = 1; i <= 4; i++)
	{
		BusWrite(i, SEG_BLANK);
	}
}

/* Show 0000 on every digit */
static void InitializeLED(void)
{
	display(0);
}
