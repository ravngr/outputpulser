#include <EEPROM.h>
#include <LiquidCrystal.h>

#include <Bounce2.h>

/* -- Helper definitions -- */
#define STR_HELPER(x)     #x
#define STR(x)            STR_HELPER(x)

/* -- Application definitions -- */
#define APP_NAME          "OutputPulse"
#define APP_VERSION_MAJOR 1
#define APP_VERSION_MINOR 1
#define APP_STR           APP_NAME " V" STR(APP_VERSION_MAJOR) "." STR(APP_VERSION_MINOR)

/* -- Application configuration -- */
#define CFG_MAX_STEP      64

#define LCD_WIDTH         16
#define LCD_HEIGHT        2
#define LCD_REFRESH       200

#define SERIAL_SPEED      115200
#define SERIAL_BUFFER_LEN 128

/* -- Pin definitions -- */
#define PIN_LCD_BUTTON    A0
#define PIN_LCD_BACKLIGHT 3
#define PIN_LCD_RS        8
#define PIN_LCD_EN        9
#define PIN_LCD_D4        4
#define PIN_LCD_D5        5
#define PIN_LCD_D6        6
#define PIN_LCD_D7        7

#define PIN_OUTPUT        2

/* -- Enums and constants -- */
typedef uint8_t count_t;
typedef bool pin_state_t;
typedef uint8_t delay_t;

// Keypad constants
#define KEYPAD_COUNT      5
#define KEYPAD_DEBOUNCE   10
#define KEYPAD_HOLD_TIME  1000

typedef enum { KEY_RIGHT, KEY_UP, KEY_DOWN, KEY_LEFT, KEY_SELECT, KEY_NONE } key_t;

// Arduino Uno + Freetronics LCD
//const static int keyAdcThreshold[KEYPAD_COUNT] = { 50, 250, 450, 650, 850 };
// Arduino Leonardo + Duinotech LCD
const static int keyAdcThreshold[KEYPAD_COUNT] = { 50, 150, 300, 500, 850 };

// UI state
typedef enum { UI_START, UI_CFG_INIT, UI_CFG_NUM, UI_CFG_STEP, UI_RUN } ui_state_t;

pin_state_t uiRedraw = true;
ui_state_t uiState = UI_START;

// Output state
bool outputState = false;

// Settings (for EEPROM)
#define EEPROM_SETTING  0
#define SETTING_VERSION ((APP_VERSION_MAJOR << 4) | (APP_VERSION_MINOR))

struct setting_struct {
  uint8_t appVersion;
  count_t numStep;
  pin_state_t initialStep;
  delay_t stepDelay[CFG_MAX_STEP];
  bool configValid;
} setting;

/* -- Hardware definition -- */
#define LCD_CHAR_UP     0
#define LCD_CHAR_DOWN   1
#define LCD_CHAR_LEFT   2
#define LCD_CHAR_RIGHT  3
#define LCD_CHAR_ON     4
#define LCD_CHAR_OFF    5

static uint8_t arrowCharUp[8] = {
  0b00100,
  0b01110,
  0b10101,
  0b00100,
  0b00100,
  0b00100,
  0b00100,
  0b00000
};

static uint8_t arrowCharDown[8] = {
  0b00100,
  0b00100,
  0b00100,
  0b00100,
  0b10101,
  0b01110,
  0b00100,
  0b00000
};

static uint8_t arrowCharLeft[8] = {
  0b00000,
  0b00100,
  0b01000,
  0b11111,
  0b01000,
  0b00100,
  0b00000,
  0b00000
};

static uint8_t arrowCharRight[8] = {
  0b00000,
  0b00100,
  0b00010,
  0b11111,
  0b00010,
  0b00100,
  0b00000,
  0b00000
};

static uint8_t arrowCharOn[8] = {
  0b11111,
  0b10001,
  0b10101,
  0b10101,
  0b10101,
  0b10001,
  0b11111,
  0b00000
};

static uint8_t arrowCharOff[8] = {
  0b11111,
  0b10001,
  0b10001,
  0b10001,
  0b10001,
  0b10001,
  0b11111,
  0b00000
};

LiquidCrystal lcd(PIN_LCD_RS, PIN_LCD_EN, PIN_LCD_D4, PIN_LCD_D5, PIN_LCD_D6, PIN_LCD_D7);


/* -- Utility functions -- */
void get_lcd_key(key_t *current_key, bool *updated) {
  static key_t lastKey = KEY_NONE;
  static long lastKeyMillis = 0;
  key_t key = KEY_NONE;

  *updated = false;

  int adcValue = analogRead(PIN_LCD_BUTTON);

  for (unsigned char n = 0; n < KEYPAD_COUNT; n++) {
    if (adcValue < keyAdcThreshold[n]) {
      key = (key_t) n;
      break;
    }
  }

  if (key != lastKey) {
    // Debounce value
    if (millis() > (lastKeyMillis + KEYPAD_DEBOUNCE)) {
      lastKey = key;
      *updated = true;
    } else {
      key = lastKey;
    }
  } else {
    lastKeyMillis = millis();
  }
  
  *current_key = key;
}

void setup() {
  /* Output setup */
  pinMode(PIN_OUTPUT, OUTPUT);
  digitalWrite(PIN_OUTPUT, LOW);
  
  /* Serial setup */
  Serial.begin(SERIAL_SPEED);
  Serial.println(APP_STR);
  Serial.println("Build: " __DATE__);
  
  /* LCD setup */
  lcd.begin(LCD_WIDTH, LCD_HEIGHT);

  // Load custom characters
  lcd.createChar(LCD_CHAR_UP, arrowCharUp);
  lcd.createChar(LCD_CHAR_DOWN, arrowCharDown);
  lcd.createChar(LCD_CHAR_LEFT, arrowCharLeft);
  lcd.createChar(LCD_CHAR_RIGHT, arrowCharRight);
  lcd.createChar(LCD_CHAR_ON, arrowCharOn);
  lcd.createChar(LCD_CHAR_OFF, arrowCharOff);

  // Print application info
  lcd.setCursor(0, 0);
  lcd.print(APP_STR);
  lcd.setCursor(0, 1);
  lcd.print(__DATE__);

  delay(1000);

  /* Read settings from EEPROM */
  EEPROM.get(EEPROM_SETTING, setting);

  // Load defaults if data is bad or out of date
  if (setting.appVersion != SETTING_VERSION) {
    setting.appVersion = SETTING_VERSION;
    setting.numStep = 0;
    setting.initialStep = false;

    // Default to zero delay times
    for (uint8_t i = 0; i < CFG_MAX_STEP; i++) {
      setting.stepDelay[i] = 1;
    }

    setting.configValid = false;

    EEPROM.put(EEPROM_SETTING, setting);
  }
}

void set_ui(ui_state_t newState) {
  uiRedraw = true;
  uiState = newState;
}

void loop() {
  bool keyChange = false;
  key_t keyState = KEY_NONE;

  uint8_t inputValue, currentStep;

  long holdTimer, runTimer;
  delay_t stepTime;

  while (1) {
    /* TODO: Process serial input */

    /* Get input */
    get_lcd_key(&keyState, &keyChange);

    /* Application state */
    switch (uiState) {
    case UI_START:
      // Select either new configuration or run old config
      if (keyChange) {
        switch (keyState) {
        case KEY_LEFT:
          // Start new configuration
          set_ui(UI_CFG_INIT);

          // Invalidate old config
          setting.configValid = false;
          break;

        case KEY_RIGHT:
          if (setting.configValid) {
            // Run with previous config
            currentStep = 0;
            holdTimer = millis();
            runTimer = millis();

            // Set initial output state
            outputState = setting.initialStep;

            if (outputState)
              digitalWrite(PIN_OUTPUT, HIGH);
            else
              digitalWrite(PIN_OUTPUT, LOW);

            set_ui(UI_RUN);
          }
          break;

        case KEY_UP:
          // Turn output ON
          outputState = true;
          digitalWrite(PIN_OUTPUT, HIGH);
          break;

        case KEY_DOWN:
          // Turn output OFF
          outputState = false;
          digitalWrite(PIN_OUTPUT, LOW);
          break;
        }

        uiRedraw = true;
      }
      break;

    case UI_CFG_INIT:
      // Get initial state
      if (keyChange) {
        switch (keyState) {
        case KEY_UP:
          // Initial output ON
          setting.initialStep = true;
          set_ui(UI_CFG_NUM);

          // Input variable setup
          inputValue = 1;
          break;

        case KEY_DOWN:
          // Initial output OFF
          setting.initialStep = false;
          set_ui(UI_CFG_NUM);

          // Input variable setup
          inputValue = 1;
          break;
        }
      }
      break;

    case UI_CFG_NUM:
      if (keyChange) {
        switch (keyState) {
        case KEY_UP:
          if (inputValue < CFG_MAX_STEP) {
            inputValue++;
            uiRedraw = true;
          }
          break;

        case KEY_DOWN:
          if (inputValue > 1) {
            inputValue--;
            uiRedraw = true;
          }
          break;

        case KEY_SELECT:
          // Save setting
          setting.numStep = inputValue;
          holdTimer = millis();
          set_ui(UI_CFG_STEP);

          // Input variable setup
          currentStep = 0;
          inputValue = 1;
          break;
        }

        uiRedraw = true;
      }
      break;

    case UI_CFG_STEP:
      // Refresh if new key is pressed or if hold timer has expired
      if (keyChange || (keyState != KEY_NONE && (millis() - holdTimer)  > KEYPAD_HOLD_TIME)) {
        switch (keyState) {
        case KEY_UP:
          if (inputValue < CFG_MAX_STEP) {
            inputValue++;
            uiRedraw = true;
          }
          break;

        case KEY_DOWN:
          if (inputValue > 1) {
            inputValue--;
            uiRedraw = true;
          }
          break;

        case KEY_LEFT:
          // Save setting
          setting.stepDelay[currentStep] = inputValue;

          if (currentStep > 0)
            currentStep--;

          inputValue = setting.stepDelay[currentStep];
          
          break;

        case KEY_RIGHT:
          // Save setting
          setting.stepDelay[currentStep] = inputValue;

          currentStep++;

          if (currentStep >= (setting.numStep - 1))
            currentStep = setting.numStep - 1;

          inputValue = setting.stepDelay[currentStep];

          break;

        case KEY_SELECT:
          // Save everything
          setting.stepDelay[currentStep] = inputValue;
          setting.configValid = true;

          // Save to EEPROM
          EEPROM.put(EEPROM_SETTING, setting);

          set_ui(UI_START);
          break;
        }

        // Update hold timer
        holdTimer = millis();

        uiRedraw = true;
      }
      break;

    case UI_RUN:
      // Abort when select is pressed
      if (keyChange && keyState == KEY_SELECT)
        set_ui(UI_START);

      // Update timer
      if (millis() >= (runTimer + 1000L * (long) setting.stepDelay[currentStep])) {
        // Toggle output
        outputState = !outputState;

        if (outputState)
          digitalWrite(PIN_OUTPUT, HIGH);
        else
          digitalWrite(PIN_OUTPUT, LOW);

        // Advance state
        currentStep++;

        // Exit when complete
        if (currentStep >= setting.numStep)
          set_ui(UI_START);

        // Reset timer
        runTimer = millis();

        uiRedraw = true;
      }

      // Refresh LCD perodically
      if (millis() >= (holdTimer + LCD_REFRESH)) {
        uiRedraw = true;
        holdTimer = millis();
      }

      break;
    } 

    /* UI drawing */
    if (uiRedraw) {
      lcd.clear();

      switch (uiState) {
      case UI_START:
        // Line 1, up: ON, down: OFF
        lcd.setCursor(0, 0);
        lcd.write(byte(LCD_CHAR_UP));
        lcd.print(" ON");

        lcd.setCursor(8, 0);
        lcd.write(byte(LCD_CHAR_DOWN));
        lcd.print(" OFF");

        // Output state
        lcd.setCursor(LCD_WIDTH - 1, 0);

        if (outputState)
          lcd.write(byte(LCD_CHAR_ON));
        else
          lcd.write(byte(LCD_CHAR_OFF));

        // Line 2, left: config, right: run
        lcd.setCursor(0, 1);
        lcd.write(byte(LCD_CHAR_LEFT));
        lcd.print(" SET");

        if (setting.configValid) {
          lcd.setCursor(8, 1);
          lcd.write(byte(LCD_CHAR_RIGHT));
          lcd.print(" RUN");
        }
        break;

      case UI_CFG_INIT:
        lcd.setCursor(0, 0);
        lcd.print("Initial state:");
        
        lcd.setCursor(0, 1);
        lcd.write(byte(LCD_CHAR_UP));
        lcd.print(" ON");

        lcd.setCursor(8, 1);
        lcd.write(byte(LCD_CHAR_DOWN));
        lcd.print(" OFF");
        break;

      case UI_CFG_NUM:
        lcd.setCursor(0, 0);
        lcd.print("Toggle count:");

        lcd.setCursor(0, 1);
        lcd.print(inputValue);
        break;

      case UI_CFG_STEP:
        lcd.setCursor(0, 0);
        lcd.print("Step ");
        lcd.print(currentStep + 1);

        if ((currentStep % 2 == 0) == setting.initialStep)
          lcd.print(" [ON]");
        else
          lcd.print(" [OFF]");

        lcd.print(":");

        lcd.setCursor(0, 1);
        lcd.print(inputValue);
        lcd.print(" sec");

        if (inputValue != 1)
          lcd.print("s");
        break;

      case UI_RUN:
        lcd.print("Step ");
        lcd.print(currentStep + 1);
        lcd.print("/");
        lcd.print(setting.numStep);

        lcd.setCursor(LCD_WIDTH - 1, 0);

        if (outputState)
          lcd.write(byte(LCD_CHAR_ON));
        else
          lcd.write(byte(LCD_CHAR_OFF));

        // Print remaining seconds at current step
        stepTime = (1000L * (long) setting.stepDelay[currentStep] - (millis() - runTimer)) / 1000 + 1;

        lcd.setCursor(0, 1);
        lcd.print("Time: ");
        lcd.print(stepTime);
        lcd.print(" sec");

        if (stepTime != 1)
          lcd.print("s");

        break;
      }

      uiRedraw = false;
    }
  }
}

