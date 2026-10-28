# MCU_project: Keypad Clock with Alarm and Stopwatch

This project turns an AVR microcontroller board into a minutes:seconds clock. It uses a 4×4 keypad for input and a 4-digit seven-segment display for output. It is the final stage of a semester of microcontroller labs. It builds on the keypad, display, and key-processing code from the earlier calculator projects (Projects 7–9) and adds Timer1 timekeeping and an interrupt-driven keypad.

The whole program is in `MCU_project.c`.

> **About the hardware description.** The source does not include a schematic. Everything in the hardware sections below is reconstructed from the register settings and the bus sequences in the code. Check it against your board's schematic before relying on it.

---

## 1. What the program does

- Keeps time as **MM:SS** (00:00 to 59:59) using a 100 Hz Timer1 interrupt, and shows it on the display continuously.
- Lets you **set the clock** and **set an alarm** from the keypad.
- **Arms or disarms the alarm** with a toggle key. When the alarm time is reached, the display flashes the current time until any key is pressed.
- Has a **stopwatch** that counts separately from the clock.
- Prints debug output (keys, settings, and the time once per second) to a PC terminal over the serial port.

---

## 2. Hardware overview

### 2.1 Microcontroller

The register names match the **ATmega128**. These include `PORTE`, `EICRB`, and `EIMSK` for INT4–INT7, and a single `TIMSK` register with `OCIE1A` at bit 4.

The board is initialized by `C4M_DEVICE_set()` from the course library `c4mlib`. This call also appears to set up the serial link that `printf` uses.

The Timer1 constant in the code (`OCR1A = 1727`) gives exactly 100 interrupts per second with an **11.0592 MHz** clock. That is a common crystal for ATmega128 boards, so it is very likely the board clock. See [Section 4.3](#43-timer1-and-timekeeping) for the math.

### 2.2 Bus architecture

The microcontroller does not drive the keypad or the display directly. Instead it uses a small external bus:

- **PORTB** is an 8-bit shared **data bus**.
- **PD4–PD6** form a 3-bit **address** that goes to a **74138** 3-to-8 decoder. The decoder picks one device.
- **PE4** is the **write strobe**. **PE3** is the **read strobe**.
- Each output device sits behind a **74374** octal D-type latch. The latch captures the data bus on a rising clock edge and holds it until the next write.

```
                      PB0–PB7  (8-bit data bus)
   ATmega128  ═══════════╤══════════╤══════════╤══════════╤══════════╗
                         │          │          │          │          ║
   PD4–PD6 ──► 74138     ▼          ▼          ▼          ▼          ▼
               decoder  [374]      [374]      [374]      [374]      [374]
                 O0 ───►  │ clk     │          │          │          │
                 O1 ──────┼───────► │ clk      │          │          │
                 O2 ──────┼─────────┼────────► │ clk      │          │
                 O3 ──────┼─────────┼──────────┼────────► │ clk      │
                 O4 ──────┼─────────┼──────────┼──────────┼────────► │ clk
                          ▼          ▼          ▼          ▼          ▼
                     keypad rows  digit 1    digit 2    digit 3    digit 4
                                (thousands) (hundreds)   (tens)     (ones)

   PE4 (WR) ──► write strobe: a rising edge clocks the selected 374
   PE3 (RD) ──► read strobe: keypad columns become readable on PB4–PB7
   PE5 (INT5) ◄── keypad interrupt line
```

### 2.3 Pin map

| Pin(s)   | Direction                        | Purpose                                                    |
|----------|----------------------------------|------------------------------------------------------------|
| PB0–PB3  | Output                           | Data bus (low nibble). During a keypad scan, row pattern.  |
| PB4–PB7  | Output (display) / Input (scan)  | Data bus (high nibble). During a keypad scan, column inputs. |
| PD4–PD6  | Output                           | 74138 address inputs (selects O0–O4)                       |
| PE3      | Output                           | Read strobe                                                |
| PE4      | Output                           | Write strobe (latch clock)                                 |
| PE5      | Input                            | External interrupt INT5 from the keypad                    |

### 2.4 Address map

| Decoder output | Device                 | Data written                     |
|----------------|------------------------|----------------------------------|
| O0             | Keypad row latch       | Row drive pattern (low nibble)   |
| O1             | Digit 1 (leftmost)     | Seven-segment code               |
| O2             | Digit 2                | Seven-segment code               |
| O3             | Digit 3                | Seven-segment code               |
| O4             | Digit 4 (rightmost)    | Seven-segment code               |

### 2.5 Bus cycles

**Write cycle** (used by `display()`, `InitializeLED()`, `FlashDark()`, and the keypad row write):

```c
REGFPT(&PORTD, 0x70, 4, addr);   // 1. Put the address on PD4–PD6
REGFPT(&PORTE, 0x18, 3, 0x00);   // 2. Pull both strobes low
REGPUT(&PORTB, 1, &data);        // 3. Put the byte on the data bus
REGFPT(&PORTE, 0x18, 3, 0x02);   // 4. Raise PE4: rising edge latches the byte
```

The 74374 captures its inputs on the rising edge of its clock. Step 4 is therefore the moment the byte is stored, and the device keeps showing it after the bus moves on.

**Read cycle** (used by the keypad scan):

```c
REGFPT(&PORTE, 0x08, 3, 0x01);           // 1. Raise PE3 (read enable)
_delay_ms(20);                           // 2. Let the line settle (also debounces)
REGFGT(&PINB, ColPins[j-4], j, &Indata); // 3. Read one column bit from PB4–PB7
REGFPT(&PORTE, 0x08, 3, 0x00);           // 4. Drop PE3
```

### 2.6 Seven-segment display

Each digit has its own latch. The code therefore writes each digit **once**, and it stays lit with no refresh loop. This is a **static** display, not a multiplexed one. A multiplexed display would light only one digit at a time and need constant refreshing.

The segment codes in `SevenSegTable[]` use this bit layout, where a 1 turns the segment on:

| Bit     | 7         | 6 | 5 | 4 | 3 | 2 | 1 | 0 |
|---------|-----------|---|---|---|---|---|---|---|
| Segment | dp (unused) | a | b | c | d | e | f | g |

```
     a
   f   b        '0' = a b c d e f   = 0b0111_1110 = 126
     g          '1' = b c           = 0b0011_0000 =  48
   e   c        '-' = g             = 0b0000_0001 =   1
     d
```

`display()` writes `0x01` (segment g only) to every digit to show `----` when a value is out of range or an entered time is invalid.

### 2.7 Keypad

This is a 4×4 matrix. The code drives one line of the matrix low at a time through the O0 latch. It then reads the four crossing lines on PB4–PB7. A pressed key connects the two lines, so the input reads 0.

`RowPins[] = {14, 13, 11, 7}` are the patterns `1110`, `1101`, `1011`, `0111`. Each one drives exactly one line low.

The code calls these driven lines "rows". The `keymap` table, however, shows that they are the physical **columns** of the keypad:

```c
char keymap[4][4] = {        // physical keypad layout:
    {'7','4','1','0'},       //   7  8  9  C
    {'8','5','2','A'},       //   4  5  6  D
    {'9','6','3','B'},       //   1  2  3  E
    {'C','D','E','F'}        //   0  A  B  F
};
```

---

## 3. The c4mlib register helpers

Every hardware access goes through four macros from `c4mlib`. As they are used in this file, they work like this:

| Call                              | Effect                                                    |
|-----------------------------------|-----------------------------------------------------------|
| `REGPUT(&REG, n, &data)`          | Write `n` bytes from `data` to `REG`                      |
| `REGGET(&REG, n, &data)`          | Read `n` bytes from `REG` into `data`                     |
| `REGFPT(&REG, mask, shift, val)`  | Write a bit field: `REG = (REG & ~mask) \| ((val << shift) & mask)` |
| `REGFGT(&REG, mask, shift, &out)` | Read a bit field: `out = (REG & mask) >> shift`           |

For example, `REGFPT(&PORTD, 0x70, 4, 3)` sets PD6:PD4 to `011` and leaves the other PORTD bits unchanged. That puts address 3 on the decoder.

---

## 4. Software architecture

### 4.1 Overview

The program is split into two short interrupt routines and a main loop that does everything else.

```
TIMER1_COMPA_vect (100 Hz)          INT5_vect (key goes down)
  count hsc → sec → min               keyRequest = 1
  count the stopwatch                       │
  check the alarm → alarmRinging            │
  secondTick = 1                            ▼
        │                   main loop ──────────────────────────────────────────┐
        │                     if keyRequest: KeyBoard() → key                   │
        │                        alarm ringing? → silence it                    │
        │                        digit?         → KeyInCumulator()              │
        │                        function key?  → Calculator()                  │
        └────────────────►    if secondTick: print the time                     │
                              UpdateDisplay(): rewrite the display if needed ◄──┘
```

The interrupt routines only update counters and set flags. All bus traffic to the keypad and the display happens in the main loop. That rule matters for two reasons:

- **The clock never loses time.** A keypad scan takes at least 320 ms. If it ran inside an ISR, interrupts would be disabled for that long. Timer1 compare matches would be dropped, because the AVR remembers only one pending request.
- **The bus is never used by two pieces of code at once.** The keypad scan and the display share PORTB and the strobe lines. If an ISR used the bus in the middle of a display write, both operations would be corrupted.

### 4.2 Startup sequence (`main`)

1. `C4M_DEVICE_set()` initializes the board.
2. PD4–PD6 and PE3–PE4 are set as outputs.
3. `InitializeLED()` shows `0000` on all four digits.
4. `SelectRow(0x00)` drives every keypad line low, so a press on any key can pull a column low and trigger INT5.
5. Timer1 is configured for a 100 Hz compare-match interrupt.
6. INT5 is configured for a **falling edge**, any stale request is cleared, and INT5 is enabled.
7. `sei()` enables global interrupts, and the main loop starts.

### 4.3 Timer1 and timekeeping

**Configuration**

| Register | Setting                                                  |
|----------|----------------------------------------------------------|
| TCCR1A   | WGM11:10 = 00                                            |
| TCCR1B   | WGM13:12 = 01, so the mode is CTC (TOP = OCR1A)          |
| TCCR1B   | CS12:10 = 011, so the prescaler is ÷64                   |
| OCR1A    | 1727                                                     |
| TIMSK    | OCIE1A = 1, which enables the compare-match A interrupt  |

In CTC mode, `TCNT1` counts from 0 up to `OCR1A`, fires `TIMER1_COMPA_vect`, and resets to 0. The interrupt rate is:

```
f_ISR = F_CLKIO / (prescaler × (1 + OCR1A))
      = 11,059,200 / (64 × 1728)
      = 100 Hz
```

The OC1A output pin is not enabled. The timer is used only for its interrupt.

**Counting in the ISR**

Every tick increments `hsc`, the hundredths-of-a-second counter. At exactly 100 ticks it resets `hsc`, advances `sec`, and carries into `min` (which wraps after 59). Doing all of this inside the ISR means the main loop never performs a read-modify-write on a counter the ISR is also changing.

On each new second the ISR also:

- compares the time with the alarm time and sets `alarmRinging` if the alarm is armed and they match, and
- sets `secondTick` so the main loop prints the time.

The stopwatch has its own hundredths counter, `swTicks`. It therefore counts whole seconds from the moment it starts, not from the clock's next second.

**Reading the counters safely**

`min`, `sec`, and the other shared values are single bytes, but the main loop often needs several of them together. It copies them inside `ATOMIC_BLOCK(ATOMIC_RESTORESTATE)` (from `<util/atomic.h>`), which turns interrupts off for a few instructions. Without that, the ISR could change `sec` to 0 and `min` to the next minute between the two reads, and the display would briefly show a time that is a minute off.

### 4.4 Keypad scanning and INT5

1. At idle, every keypad line is driven low. A key press pulls a column low, which produces a falling edge on PE5 (INT5).
2. `ISR(INT5_vect)` only sets `keyRequest`.
3. The main loop sees `keyRequest` and calls `KeyBoard()`.
4. `KeyBoard()` writes each drive pattern to the O0 latch with `SelectRow()` and reads each column with `ReadColumn()`. The read raises PE3, waits 20 ms to settle and debounce, reads the column bit, and drops PE3.
5. When a column reads 0, `KeyBoard()` waits until it reads 1 again, so a key is registered **on release**. It then stops scanning.
6. Before returning, `KeyBoard()` restores the idle pattern `0000`, so any key can trigger the next interrupt.
7. Changing the drive pattern during the scan also toggles the INT5 line. The main loop therefore clears `keyRequest` and the `INTF5` flag after the scan, so those edges are not mistaken for new presses.

### 4.5 Key processing

**`KeyResolver()`** returns `number` for a digit (and stores its value) or `sign` for anything else.

**`KeyInCumulator()`** only accepts digits while setting the clock or the alarm. It keeps the last four digits typed, shifting left like a calculator:

```
type 1 → 0001,  2 → 0012,  3 → 0123,  0 → 1230   (12:30)
```

**`Calculator()`** handles the function keys and owns the system mode:

| Mode             | What the display shows               |
|------------------|--------------------------------------|
| `MODE_CLOCK`     | Current time (flashing while the alarm rings) |
| `MODE_SET_CLOCK` | Digits being entered, steady         |
| `MODE_SET_ALARM` | Digits being entered, flashing       |
| `MODE_STOPWATCH` | Stopwatch time                       |

| Key | Action                                                                     |
|-----|----------------------------------------------------------------------------|
| D   | Enter `MODE_SET_CLOCK` with an empty entry                                 |
| E   | Enter `MODE_SET_ALARM` with an empty entry                                 |
| A   | In a setup mode, check the entry. If the minutes and seconds are both 0–59, apply it and return to `MODE_CLOCK`. Otherwise show `----` and stay in the setup mode. |
| B   | Discard the entry, stop the stopwatch, and return to `MODE_CLOCK`. Saved settings are kept. |
| C   | Toggle the alarm on or off                                                 |
| F   | 1st press: reset and start the stopwatch. 2nd press: stop it and hold the time. 3rd press: return to `MODE_CLOCK`. |

Setting the clock also resets `hsc`, so the new time starts on a full second.

### 4.6 Display

**`UpdateDisplay()`** runs on every pass of the main loop:

1. It copies the shared counters atomically.
2. It decides what to show from the mode: a value from 0 to 9999 (or −1 for `----`) and whether the digits should be blanked for flashing.
3. Flashing uses the tick counter: the digits are on while `hsc < 50` and off while `hsc ≥ 50`. That gives a 1 Hz blink with a 50% duty cycle.
4. It rewrites the display **only when the value or the blank state changes**. Because each digit has its own latch, the display holds its contents between writes. Writing only on a change keeps the bus quiet.
5. After writing, it returns PORTB to the idle direction (PB0–PB3 outputs, PB4–PB7 inputs), which is the same state the keypad scan expects.

**`display()`** splits the value into four decimal digits and writes each segment code to O1–O4 with `BusWrite()`. **`FlashDark()`** writes `0x00` to all four digits. **`InitializeLED()`** shows `0000`.

**`BusWrite()`** is the write cycle from [Section 2.5](#25-bus-cycles) in one function: set all of PORTB as outputs, put the address on PD4–PD6, pull the strobes low, put the byte on PORTB, and raise PE4.

---

## 5. How to use it

**Set the clock to 12:30**

```
D  1  2  3  0  A
```

**Set an alarm for 12:45 and arm it**

```
E  1  2  4  5  A     then     C
```

**Use the stopwatch**

```
F   (start)     F   (stop)     F   (back to the clock)
```

**Cancel what you are doing**

```
B
```

When the alarm goes off, the display flashes the current time. Press any key to silence it. The alarm stays armed and will ring again at the same time in the next hour, because the clock wraps at 59:59. Press C to disarm it.

---

## 6. Debug output

When connected to a PC terminal, the program prints:

- `Time MM:SS` once per second, followed by `ALARM` while the alarm is ringing.
- `Key X` for every key press.
- A confirmation for each action: `Clock set to …`, `Alarm set to …`, `Alarm on`, `Alarm off`, `Alarm stopped`, `Abandoned`, and `Invalid time`.

There is no `printf` inside any ISR. Printing over the serial port is slow and would block the interrupt.

---

## 7. Changes from the original lab version

This program is a reworked version of the original Project 10-1 lab code. This table lists each problem in the original and how `MCU_project.c` fixes it.

| # | Problem in the original | Fix |
|---|-------------------------|-----|
| 1 | The display updated only on key presses, because the display call in `main` was commented out. | `UpdateDisplay()` runs every loop and follows the clock. |
| 2 | The alarm was compared only inside `Calculator()`, which ran only after a key press. | The timer ISR compares the alarm on every second. |
| 3 | `hsc > 100` made each second 101 ticks long, so the clock ran 1% slow. | The ISR counts exactly 100 ticks per second. |
| 4 | The keypad scan ran inside the INT5 ISR for 320 ms or more, so Timer1 interrupts were lost on every key press. | The ISR only sets a flag. The scan runs in the main loop. |
| 5 | The F key tested `KeyIn == 69`, the same code as E, so the stopwatch could not be reached. | F is recognized as `'F'`. |
| 6 | INT5 was set to a low-level trigger even though the comment said falling edge. | `EICRB` ISC51:50 = `10` (falling edge). Stale requests are cleared after each scan. |
| 7 | The scan left `0111` in the row latch, which could stop most keys from triggering INT5. | `KeyBoard()` restores `0000` before it returns. |
| 8 | Digit entry stopped once the buffer passed 59, so 12:30 could not be entered. | The entry keeps the last four digits, and the minutes and seconds are checked separately when A is pressed. |
| 9 | `Calculator()` received `Flag_eq` and `Operator` by value, and `main` hid the global `Operator` behind a local one. Settings were re-applied on later key presses, C toggled the alarm on every key, and B did not leave the mode. | A single `mode` variable, changed only by `Calculator()`. Each function key acts once. |
| 10 | The stopwatch reset `min` and `sec`, erasing the clock. | The stopwatch has its own counters and keeps counting separately. |
| 11 | `InitializeLED()` and `FlashDark()` always selected digit 4. | Both write all four digits. |
| 12 | Alarm setup showed the current time without flashing, which did not match the spec. | Alarm setup flashes the digits being entered. |

Other cleanups:

- The repeated bus sequences are now `BusWrite()`, `SelectRow()`, and `ReadColumn()`.
- Values shared with ISRs are `volatile` and read inside atomic blocks.
- There is no `printf` inside any ISR.

**Behavior changes to be aware of**

- **B (abandon)** now cancels only the current entry or mode. The original also erased the saved alarm and turned it off. To get that back, add `alarmArmed = 0; alarmMin = 0; alarmSec = 0;` to the `KEY_ABANDON` case.
- **The stopwatch** uses three presses of F (start, stop, return) instead of the original odd/even toggle.

---

## 8. Building

- The project uses an Atmel Studio (now Microchip Studio) layout. Add `MCU_project.c` to a project for the ATmega128.
- Add the course library `c4mlib` (the header `c4mlib.h` and its library file) to the project.
- Compile as C99 or later (`-std=gnu99`, which is the Atmel Studio default). `ATOMIC_BLOCK` requires it.
- Make sure `F_CPU` matches the board clock (11.0592 MHz is the likely value). `_delay_ms()` and the Timer1 calculation both depend on it.
- Open a serial terminal at the baud rate the course library uses to see the debug output.

The fixed version compiles without warnings under `avr-gcc -mmcu=atmega128 -std=gnu99 -Wall -Wextra` against a stand-in `c4mlib.h`. It has **not** been run on the real board.
