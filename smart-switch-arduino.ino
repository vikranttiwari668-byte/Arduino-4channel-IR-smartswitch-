#include <IRremote.h>
#include <EEPROM.h>
#include <DHT.h>

// --- Verified IR Remote Hex Codes ---
#define IR1        0xF20DFF00
#define IR2        0xE619FF00
#define IR3        0xE41BFF00 // Fan Manual Toggle
#define IR4        0xFE01FF00 // Light Manual Toggle
#define IR_MUTE    0xFC03FF00 // Auto Mode Enable/Disable Toggle
#define IR_POWER   0xF609FF00 // All OFF + 2 Min Baad Auto Mode
#define IR_OK      0xFA05FF00 // Sleep / Night Mode Toggle
#define IR_VOL_UP  0xE11EFF00 // Sequential Turn ON (1 -> 2 -> 3 -> 4)
#define IR_VOL_DN  0xF50AFF00 // Sequential Turn OFF (4 -> 3 -> 2 -> 1)

// --- Hardware Pins ---
const byte IR_PIN = 2;
const byte BUZZER_PIN = 9;
const byte relayPins[4] = {4, 5, 6, 7};
// Relay index 2 (Pin 6) = Fan
// Relay index 3 (Pin 7) = Light

// --- Sensor Pins & Config ---
const byte PIR_PIN = A0;
const byte LDR_PIN = A1; // Window par bahar ki taraf face karta hua LDR
#define DHT_PIN 8
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// --- Thresholds & Hysteresis ---
const int LDR_DARK_THRESHOLD = 500;            // Isse kam analog value = Night/Dark
const float TEMP_HIGH_THRESHOLD = 28.0;        // Fan ON threshold
const float TEMP_LOW_THRESHOLD  = 27.0;        // Fan OFF threshold (Hysteresis Buffer)

const unsigned long AUTO_OFF_TIMEOUT = 180000;         // 3 minute no-motion timeout (ms)
const unsigned long POWER_OFF_REARM_TIME = 120000;     // Power button dabane ke 2 minute baad Auto Mode (ms)
const unsigned long MANUAL_OVERRIDE_DURATION = 900000; // Manual remote press ke baad 15 min override lock (ms)

// --- Modes ---
// 0: Manual (Automation Disabled)
// 1: Fully Automatic (Motion -> Light & Fan Auto ON, Auto OFF)
// 2: Sleep/Night Mode (Fan ON Continuous, Motion Light OFF)
byte currentMode = 1; 

// --- State & Timing Variables ---
bool relayState[4] = {false, false, false, false};
bool buzzerEnabled = true;
byte volumeLevel = 4; // 1 to 5

int seqUpIndex = 0;
int seqDownIndex = 3;

bool powerTimerActive = false;
unsigned long powerOffTimestamp = 0;

// Override flags: manual remote control par automation pause karne ke liye
bool lightManualOverride = false;
unsigned long lightOverrideTimestamp = 0;

bool fanManualOverride = false;
unsigned long fanOverrideTimestamp = 0;

unsigned long lastMotionTime = 0;
unsigned long lastSensorReadTime = 0;
float currentTemp = 0.0;
int currentLdr = 0;

unsigned long lastActionTime = 0;
const unsigned long ACTION_DEBOUNCE_DELAY = 250;

// High-Power Conflict-Free Tone Engine
void soundTone(unsigned int freq, unsigned long durationMs) {
  if (!buzzerEnabled || volumeLevel == 0) return;

  unsigned long period = 1000000UL / freq;
  unsigned long highTime = (period * volumeLevel) / 10;
  if (highTime < 2) highTime = 2;
  unsigned long lowTime = period - highTime;
  unsigned long cycles = (freq * durationMs) / 1000UL;

  for (unsigned long i = 0; i < cycles; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delayMicroseconds(highTime);
    digitalWrite(BUZZER_PIN, LOW);
    delayMicroseconds(lowTime);
  }
}

// --- Melodies & Audio Tones ---
void toneStartupBoot() {
  if (!buzzerEnabled) return;
  soundTone(523, 160); delay(50);
  soundTone(659, 160); delay(50);
  soundTone(784, 180); delay(60);
  soundTone(1046, 350);
}

void toneTurnOn() {
  soundTone(1760, 40); delay(25);
  soundTone(2217, 50); delay(25);
  soundTone(2637, 70);
}

void toneTurnOff() {
  soundTone(850, 40);
}

void toneAllOff() {
  soundTone(1568, 70); delay(35);
  soundTone(1318, 90); delay(35);
  soundTone(1046, 180);
}

void toneSequenceUp(int step) {
  unsigned int freqs[4] = {1046, 1318, 1568, 2093};
  soundTone(freqs[step], 80);
}

void toneSequenceDown(int step) {
  unsigned int freqs[4] = {880, 784, 659, 523};
  soundTone(freqs[3 - step], 80);
}

void toneAutoModeEnabled() {
  soundTone(523, 180); delay(50);
  soundTone(659, 180); delay(50);
  soundTone(784, 180); delay(50);
  soundTone(1046, 220); delay(60);
  soundTone(1318, 250); delay(60);
  soundTone(1568, 450);
}

void toneAutoModeDisabled() {
  soundTone(1568, 200); delay(60);
  soundTone(1318, 200); delay(60);
  soundTone(1046, 200); delay(60);
  soundTone(784, 250); delay(80);
  soundTone(523, 500);
}

void toneSleepModeOn() {
  soundTone(1046, 200); delay(80);
  soundTone(784, 250); delay(90);
  soundTone(659, 300); delay(100);
  soundTone(523, 600);
}

void toneSleepModeOff() {
  soundTone(523, 150); delay(60);
  soundTone(659, 150); delay(60);
  soundTone(1046, 300);
}

void toneAutoRearmed() {
  soundTone(1760, 80); delay(60);
  soundTone(2637, 120);
}

// --- Relay Control (Active LOW) ---
void setRelay(int index, bool state) {
  relayState[index] = state;
  digitalWrite(relayPins[index], state ? LOW : HIGH);
  EEPROM.update(index, state ? 1 : 0);
}

void toggleRelay(int index) {
  bool targetState = !relayState[index];
  setRelay(index, targetState);
  if (targetState) {
    toneTurnOn();
  } else {
    toneTurnOff();
  }
}

void setup() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(PIR_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  dht.begin();

  // Load Saved Mode (Address 4)
  byte savedMode = EEPROM.read(4);
  if (savedMode <= 2) {
    currentMode = savedMode;
  } else {
    currentMode = 1;
    EEPROM.update(4, currentMode);
  }

  // Load Saved Volume (Address 5)
  byte savedVol = EEPROM.read(5);
  if (savedVol >= 1 && savedVol <= 5) {
    volumeLevel = savedVol;
  } else {
    volumeLevel = 4;
    EEPROM.update(5, volumeLevel);
  }

  // Restore Relays (Addresses 0-3)
  for (int i = 0; i < 4; i++) {
    pinMode(relayPins[i], OUTPUT);
    byte savedState = EEPROM.read(i);
    relayState[i] = (savedState == 1);
    digitalWrite(relayPins[i], relayState[i] ? LOW : HIGH);
  }

  toneStartupBoot();
  IrReceiver.begin(IR_PIN, ENABLE_LED_FEEDBACK);
}

void loop() {
  unsigned long now = millis();

  // --- 1. Periodic Sensor Reading (Har 2 Second Me) ---
  if (now - lastSensorReadTime > 2000) {
    lastSensorReadTime = now;
    float t = dht.readTemperature();
    if (!isnan(t)) {
      currentTemp = t;
    }
    // Filtered window ambient light
    currentLdr = (currentLdr * 3 + analogRead(LDR_PIN)) / 4;
  }

  // --- 2. Power Button 2-Min Rearm Timer ---
  if (powerTimerActive) {
    if (now - powerOffTimestamp >= POWER_OFF_REARM_TIME) {
      powerTimerActive = false;
      currentMode = 1; // Return to Fully Automatic
      EEPROM.update(4, currentMode);
      // Clear any manual overrides
      lightManualOverride = false;
      fanManualOverride = false;
      toneAutoRearmed();
    }
  }

  // --- 3. Manual Override Timeout Check (15 min) ---
  if (lightManualOverride && (now - lightOverrideTimestamp >= MANUAL_OVERRIDE_DURATION)) {
    lightManualOverride = false;
  }
  if (fanManualOverride && (now - fanOverrideTimestamp >= MANUAL_OVERRIDE_DURATION)) {
    fanManualOverride = false;
  }

  // --- 4. Smart Automation Logic ---
  bool motionDetected = (digitalRead(PIR_PIN) == HIGH);

  // >>> MODE 1: FULLY AUTOMATIC MODE <<<
  if (currentMode == 1) {
    if (motionDetected) {
      lastMotionTime = now;

      // Rule A: Light Automation (Respects manual override)
      if (!lightManualOverride) {
        if (currentLdr < LDR_DARK_THRESHOLD) {
          if (!relayState[3]) {
            setRelay(3, true);
            toneTurnOn();
          }
        }
      }

      // Rule B: Fan Automation with Hysteresis (Respects manual override)
      if (!fanManualOverride) {
        if (currentTemp >= TEMP_HIGH_THRESHOLD) {
          if (!relayState[2]) {
            setRelay(2, true);
            toneTurnOn();
          }
        } else if (currentTemp < TEMP_LOW_THRESHOLD) {
          if (relayState[2]) {
            setRelay(2, false);
            toneTurnOff();
          }
        }
      }
    } else {
      // No movement for 3 minutes -> Turn OFF non-overridden appliances
      if (now - lastMotionTime > AUTO_OFF_TIMEOUT) {
        if (relayState[3] && !lightManualOverride) setRelay(3, false);
        if (relayState[2] && !fanManualOverride)   setRelay(2, false);
      }
    }
  }
  // >>> MODE 2: SLEEP / NIGHT MODE <<<
  else if (currentMode == 2) {
    // Sote waqt Fan continuously ON rahega
    if (!relayState[2] && !fanManualOverride) {
      setRelay(2, true);
    }
    // Movement hone par bhi Light auto-trigger nahi hogi
  }

  // --- 5. IR Remote Controls ---
  if (IrReceiver.decode()) {
    uint32_t code = IrReceiver.decodedIRData.decodedRawData;

    if (!(IrReceiver.decodedIRData.flags & IRDATA_FLAGS_IS_REPEAT)) {
      if (code != 0 && (now - lastActionTime > ACTION_DEBOUNCE_DELAY)) {
        lastActionTime = now;

        switch (code) {
          case IR1: toggleRelay(0); break;
          case IR2: toggleRelay(1); break;

          case IR3: // Fan Manual Toggle
            toggleRelay(2);
            fanManualOverride = true;
            fanOverrideTimestamp = now;
            break;

          case IR4: // Light Manual Toggle
            toggleRelay(3);
            lightManualOverride = true;
            lightOverrideTimestamp = now;
            break;

          case IR_POWER: // Turn OFF All + 2 Min Auto Rearm Trigger
            for (int i = 0; i < 4; i++) {
              setRelay(i, false);
            }
            seqUpIndex = 0;
            seqDownIndex = 3;
            powerTimerActive = true;
            powerOffTimestamp = now;
            lightManualOverride = false;
            fanManualOverride = false;
            toneAllOff();
            break;

          case IR_OK: // Sleep / Night Mode Toggle
            if (currentMode != 2) {
              currentMode = 2; // Sleep Mode ON
              toneSleepModeOn();
            } else {
              currentMode = 1; // Normal Auto Mode
              toneSleepModeOff();
            }
            EEPROM.update(4, currentMode);
            powerTimerActive = false;
            lightManualOverride = false;
            fanManualOverride = false;
            break;

          case IR_MUTE: // Auto Mode Toggle (Manual vs Auto)
            if (currentMode != 0) {
              currentMode = 0; // Automation OFF
              toneAutoModeDisabled();
            } else {
              currentMode = 1; // Automation ON
              toneAutoModeEnabled();
            }
            EEPROM.update(4, currentMode);
            powerTimerActive = false;
            lightManualOverride = false;
            fanManualOverride = false;
            break;

          case IR_VOL_UP: // Sequence ON (1 -> 2 -> 3 -> 4)
            if (seqUpIndex < 4) {
              setRelay(seqUpIndex, true);
              toneSequenceUp(seqUpIndex);
              seqUpIndex++;
            }
            seqDownIndex = 3;
            break;

          case IR_VOL_DN: // Sequence OFF (4 -> 3 -> 2 -> 1)
            if (seqDownIndex >= 0) {
              setRelay(seqDownIndex, false);
              toneSequenceDown(seqDownIndex);
              seqDownIndex--;
            }
            seqUpIndex = 0;
            break;
        }
      }
    }
    IrReceiver.resume();
  }
}
