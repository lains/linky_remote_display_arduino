//#define SERIAL_RX_BUFFER_SIZE 512
#include <Wire.h>
#include <Adafruit_RGBLCDShield.h>
#include <utility/Adafruit_MCP23017.h>
#include <avr/wdt.h>
#include "TIC/Unframer.h"
#include "TIC/DatasetExtractor.h"
#include "TIC/DatasetView.h"
#include "TicFrameParser.h"
#include "TicContext.h"
#include <stdint.h> // For INT32_MIN

Adafruit_RGBLCDShield lcd = Adafruit_RGBLCDShield();

// These #defines make it easier to set the backlight color
#define RED 0x1
#define YELLOW 0x3
#define GREEN 0x2
#define TEAL 0x6
#define BLUE 0x4
#define VIOLET 0x5
#define WHITE 0x7

#define MONOCHROME_DISPLAY

#ifndef MONOCHROME_DISPLAY
#define BACK_LIGHT_ON TEAL
#else
#define BACK_LIGHT_ON RED // MONO backlight ON is on RED line
#endif

#define ELEC_ICON_IDX 0
#define FULL_SQUARE_IDX 1
/* Icon created with https://www.quinapalus.com/hd44780udg.html (character size: 5x8) */
uint8_t icons[2][8] = { { 0x8, 0x4, 0x2, 0x4, 0x8, 0x5, 0x3, 0x7 },
  { 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f, 0x1f}
};

constexpr uint16_t NO_SERIAL_INIT_TIMEOUT_MS = 2000; /*!< How long (in ms) should we delay (since boot time) the serial setup when a user action is performed at boot time? (max value=655350/LCD_WIDTH, thus 40s) */
constexpr uint16_t TIC_PROBE_FAIL_TIMEOUT_MS = 10000; /*!< How long (in ms) should we wait (since boot time) for TIC synchronization before issuing a warning messages */
#define LCD_WIDTH 16
#define LCD_HEIGHT 2

/* Optimized version taken from https://lemire.me/blog/2021/05/28/computing-the-number-of-digits-of-an-integer-quickly/ */
#if 0
static int int_log2(uint32_t x) { return 31 - __builtin_clz(x|1); }

static uint8_t uint32ToNbDigits(uint32_t value) {
    static uint32_t table[] = {9, 99, 999, 9999, 99999, 999999, 9999999, 99999999, 999999999};
    int y = (9 * int_log2(x)) >> 5;
    y += x > table[y];
    return y + 1;

}
#endif

static uint8_t uint32ToNbDigits(uint32_t value) {
  uint8_t result = 0;
  result += (value/1000000000 != 0)?1:0;
  result += (value/100000000 != 0)?1:0;
  result += (value/10000000 != 0)?1:0;
  result += (value/1000000 != 0)?1:0;
  result += (value/100000 != 0)?1:0;
  result += (value/10000 != 0)?1:0;
  result += (value/1000 != 0)?1:0;
  result += (value/100 != 0)?1:0;
  result += (value/10 != 0)?1:0;
  result++; /* Even 0 will be represented as one digit, so the unit value does not matter */
  return result;
}
#define uint32ToNbDigits(i) (i<10?1:(i<100?2:(i<1000?3:(i<10000?4:(i<100000?5:(i<1000000?6:7))))))
/**
   Boot progress and TIC decoding state machine
*/
#define STAGE_WAIT_RELEASE_INIT 0
#define STAGE_WAIT_RELEASE_CHAR_CREATED 1
#define STAGE_WAIT_RELEASE_DISPLAY_DONE 2
#define STAGE_SERIAL_INIT 10
#define STAGE_TIC_INIT 11
#define STAGE_TIC_PROBE 20  /*!< Waiting to start synchronization with TIC stream */
#define STAGE_TIC_SYNC_FAIL 21  /*!< No TIC stream or impossible to decode TIC stream */
#define STAGE_TIC_IN_SYNC 30 /*!< Synchronized to TIC stream, currently receiving TIC data */
#define STAGE_TIC_IN_SYNC_RUNNING_LATE 31 /*!< Synchronized to TIC stream, currently receiving TIC data but we are running late in decoding incoming bytes */

g_ctx_t ctx ; /*!< Global context storage */

void(*swReset) (void) = 0;  /*!< declare reset fuction at address 0 */

/**
   @brief Setup the board at initial boot
*/
void setup() {
  Serial.begin(9600); /* Ephemeral serial init for program upload */
  ctx.boot.startupTime = millis();
  ctx.boot.nbDotsProgress = 0;
  ctx.tic.lateTicDecodeCount = 0;
  ctx.tic.ticUpdates = 0;
  ctx.tic.lastValidWithdrawnPower = INT32_MIN;
  ctx.lcd.displayedPower = INT32_MIN;
  ctx.tic.lastTicDecodeState = TIC_NO_SYNC;
  ctx.lcd.nbCharsOnLine0 = LCD_WIDTH; /* LCD display assumed to be full of characters to clear */
  ctx.tic.beat = false;
  pinMode(LED_BUILTIN, OUTPUT);
  lcd.begin(LCD_WIDTH, LCD_HEIGHT); /* Initialize the LCD display: 16x2 */
  lcd.setBacklight(WHITE);
  if (lcd.readButtons() & BUTTON_SELECT) {
    /* BUTTON_SELECT pressed at startup, do not start the serial port if button is maintained */
    ctx.stage = STAGE_WAIT_RELEASE_INIT;
    /* Keep button pressed to swtch to programming mode */
    lcd.setCursor(0, 0);
    lcd.print("Programming mode");
  }
  else {
    ctx.stage = STAGE_SERIAL_INIT;
  }
}

/**
   @brief Enter an infinite chase effect display on the LCD when switched to upload mode
*/
void infiniteLoopWaitUpdate() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Waiting 4 upload");
  while (1) {
    for (unsigned int i = 0; i < LCD_WIDTH - 1; i++) {
      lcd.setCursor(i, 1);
      lcd.print('*');
      delay(1000);
      lcd.setCursor(i, 1);
      lcd.print(' ');
    }
    for (unsigned int i = LCD_WIDTH - 1; i > 0; i--) {
      lcd.setCursor(i, 1);
      lcd.print('*');
      delay(1000);
      lcd.setCursor(i, 1);
      lcd.print(' ');
    }
  }
}

/**
   @brief Display a progress bar when progressing towards upload mode at startup

   @note This requires continuous user interaction (override) to continue progression towards upload mode, and should be called repeatedly in a loop
   @warning If upload mode is reached, this function will never return. If progressing to upload mode, this function will update the display and return
*/
void bootModeCheckAndProgressDisplay(ctx_boot_t &boot_ctx) {
  int timeNow = millis();
  if (timeNow > boot_ctx.startupTime) {
    uint16_t timeDelta = timeNow - boot_ctx.startupTime;
    if (timeDelta < NO_SERIAL_INIT_TIMEOUT_MS) {
      /* Note we divide both timeDelta and NO_SERIAL_INIT_TIMEOUT_MS by 10 here to avoid 16-bit overflow when multiplying by timeDelta (uint16_t) by LCD_WIDTH */
      uint8_t currentNbDotsProgress = ((uint16_t)timeDelta / 10 * LCD_WIDTH) / (NO_SERIAL_INIT_TIMEOUT_MS / 10);
      if (boot_ctx.nbDotsProgress < currentNbDotsProgress) {
        lcd.setCursor(currentNbDotsProgress - 1, 1);
        lcd.write(FULL_SQUARE_IDX);
        boot_ctx.nbDotsProgress = currentNbDotsProgress;
      }
    }
    else {  /* Delayed serial action has been maintained from the very start of boot and during the whole NO_SERIAL_INIT_TIMEOUT_MS period, assume we will stay in the bootloader without decoding TIC */
      Serial.println("Stay in boot");
      infiniteLoopWaitUpdate();
      swReset();
      while (true) {}
    }
  } /* else there was a count error, continue booting */
}

TicFrameParser ticParser(ctx);

TIC::Unframer ticUnframer(TicFrameParser::unwrapInvokeOnFrameNewBytes,
                          TicFrameParser::unwrapInvokeOnFrameComplete,
                          (void *)(&ticParser));

/**
   @brief Update the values displayed on the LCD screen
   @param ctx The current context
*/
void updateDisplay(g_ctx_t& ctx) {
  static uint32_t lastDisplayedFrameNo = 0;
  if (ctx.stage < STAGE_TIC_IN_SYNC) {
    lcd.clear();  /* Switching to sync then to power display mode */
    ctx.stage = STAGE_TIC_IN_SYNC;
  }
  else if (ctx.stage == STAGE_TIC_IN_SYNC) {  /* Display only if STAGE_TIC_IN_SYNC, not even when STAGE_TIC_IN_SYNC_RUNNING_LATE, as we don't have enought time to  */
    if (ctx.tic.lastValidWithdrawnPower != INT32_MIN) {
      if (ctx.lcd.displayedPower != ctx.tic.lastValidWithdrawnPower) {
        /*
        if (ctx.tic.lateTicDecodeCount>0) {
          if (ctx.tic.lateTicDecodeCount>26)
            ctx.tic.lateTicDecodeCount = 0;
          lastTicDecode = 'a' + ctx.tic.lateTicDecodeCount;
        }*/
        char lastTicDecode = ' ';
        if (lastDisplayedFrameNo != ctx.tic.nbFramesParsed) {
          uint8_t skipped = (uint8_t)(ctx.tic.nbFramesParsed - lastDisplayedFrameNo);
          if (skipped > 1) { /* 1 is normal, we are displaying the next frame */
            skipped--; /* Count the number of skipped frames */
            skipped--; /* 1 skipped corresponds to a */
            if (skipped<26)
              lastTicDecode = 'a' + skipped;
            else
              lastTicDecode = '*';  /* Overflow */
          }
        }
        lcd.setCursor(0, 0);
        uint32_t absLastValidWithdrawnPower;
        uint8_t displayedChars = 0;
        if (ctx.tic.lastValidWithdrawnPower < 0) {
          lcd.print('~');
          displayedChars++; /* For the '~' character */
          lcd.print('-');
          displayedChars++; /* For the leading '-' sign */
          absLastValidWithdrawnPower = -ctx.tic.lastValidWithdrawnPower;
        }
        else {
          absLastValidWithdrawnPower = ctx.tic.lastValidWithdrawnPower;
        }
        lcd.print(absLastValidWithdrawnPower);
        displayedChars += uint32ToNbDigits(absLastValidWithdrawnPower);
        
        lcd.print('W');
        displayedChars++;
        
        if (lastTicDecode != ' ') {
          lcd.print(lastTicDecode); /* Dump the late TIC decode count letter right after character 'W' */
          displayedChars++;
        }
        if (displayedChars > LCD_WIDTH) /* Foolproof */
          displayedChars = LCD_WIDTH;
        
        if (ctx.lcd.nbCharsOnLine0 < displayedChars) {
          ctx.lcd.nbCharsOnLine0 = displayedChars;
        }
        else {
          while (ctx.lcd.nbCharsOnLine0 > displayedChars) {  /* We have previous chars to clean up */
            lcd.print(' ');
            ctx.lcd.nbCharsOnLine0--;
          }
        }
        wdt_reset();
      }
      lastDisplayedFrameNo = ctx.tic.nbFramesParsed;
      ctx.lcd.displayedPower = ctx.tic.lastValidWithdrawnPower;
    }
  }
}

/**
   @brief Main loop
*/
void loop() {
  static byte serialRxBuf[64];

  if (ctx.stage < STAGE_SERIAL_INIT) {
    while (lcd.readButtons() & BUTTON_SELECT) {
      if (ctx.stage < STAGE_WAIT_RELEASE_CHAR_CREATED) {
        lcd.createChar(FULL_SQUARE_IDX, icons[FULL_SQUARE_IDX]);
        ctx.stage = STAGE_WAIT_RELEASE_CHAR_CREATED;
      }
      bootModeCheckAndProgressDisplay(ctx.boot);
    } /* No button pressed before end of timeout, continue booting */
    ctx.stage = STAGE_SERIAL_INIT;  /* Delayed serial action has been stopped before the end of NO_SERIAL_INIT_TIMEOUT_MS period, continue serial initialization */
  }
  if (ctx.stage == STAGE_SERIAL_INIT) {
    Serial.println("Swicthing serial port to TIC");
    Serial.begin(9600, SERIAL_7E1);
    wdt_enable(WDTO_500MS);
    lcd.createChar(ELEC_ICON_IDX, icons[ELEC_ICON_IDX]);
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Syncing...");
    ctx.stage = STAGE_TIC_PROBE;
    //    lcd.setCursor(0, 1);
  }
  if (ctx.stage >= STAGE_TIC_PROBE) {
    wdt_reset();
    int waitingRxBytes = Serial.available();  /* Is there incoming data pending, and how much (in bytes) */
    if (waitingRxBytes > 0) {
      if (ctx.tic.lastTicDecodeState == TIC_IN_SYNC && ctx.stage == STAGE_TIC_IN_SYNC && waitingRxBytes > (SERIAL_RX_BUFFER_SIZE * 3 / 4)) { /* Less that 1/4 of incoming buffer is available, we are running late */
        ctx.stage = STAGE_TIC_IN_SYNC_RUNNING_LATE;
        ctx.tic.lateTicDecodeCount++;
      }

      /////
      if (waitingRxBytes > sizeof(serialRxBuf))
        waitingRxBytes = sizeof(serialRxBuf);
      Serial.readBytes(serialRxBuf, waitingRxBytes);
      unsigned int processedBytesCount = ticUnframer.pushBytes(serialRxBuf, waitingRxBytes);
      if (processedBytesCount < waitingRxBytes) {
        unsigned int lostBytesCount = waitingRxBytes - processedBytesCount;
        if (lostBytesCount > static_cast<unsigned int>(-1)) {
          lostBytesCount = static_cast<unsigned int>(-1); /* Does not fit in an unsigned int! */
        }
        if (static_cast<unsigned int>(-1) - lostBytesCount < ctx.tic.lostTicBytes) {
          /* Adding lostBytesCount will imply an overflow an overflow of our counter */
          ctx.tic.lostTicBytes = static_cast<uint32_t>(-1); /* Maxmimize our counter, we can't do better than this */
        }
        else {
          ctx.tic.lostTicBytes += lostBytesCount;
        }
      }
      tic_state_t newTicDecodeState = (ticUnframer.isInSync()?TIC_IN_SYNC:TIC_NO_SYNC);

      wdt_reset();
      if (newTicDecodeState == TIC_IN_SYNC)
        updateDisplay(ctx);
      ctx.tic.lastTicDecodeState = newTicDecodeState;
    }
    else {  /* No waiting RX byte... we processed every TIC byte, we are running early */
      if (ctx.stage == STAGE_TIC_IN_SYNC_RUNNING_LATE)
        ctx.stage = STAGE_TIC_IN_SYNC;
    }
    if (ctx.stage == STAGE_TIC_PROBE && ctx.tic.lastTicDecodeState != TIC_IN_SYNC) {
      if (millis() - ctx.boot.startupTime > TIC_PROBE_FAIL_TIMEOUT_MS) {
        ctx.stage = STAGE_TIC_SYNC_FAIL;
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("No signal!");
        digitalWrite(LED_BUILTIN, HIGH);
        wdt_reset();
      }
    }
  }
}
