#include "ButtonManager.h"
#include "Config.h"
#include "DisplayManager.h"
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>

using namespace Adafruit_LittleFS_Namespace;

#define DEBOUNCE_DELAY_MS   30
#define LONG_PRESS_DELAY_MS 800

extern const uint32_t g_ADigitalPinMap[];

ButtonManager& ButtonManager::getInstance() {
    static ButtonManager instance;
    return instance;
}

ButtonManager::ButtonManager() {
    // Initialize buttons states
    _prevBtn = {PIN_BTN_PREV, true, true, 0, 0, false, BTN_NONE};
    _nextBtn = {PIN_BTN_NEXT, true, true, 0, 0, false, BTN_NONE};
    _selectBtn = {PIN_BTN_SELECT, true, true, 0, 0, false, BTN_NONE};
}

void ButtonManager::begin() {
    // nice!nano VCC power pin (Pin 13 / P0.13) MUST be set to OUTPUT and LOW
    // on boot to turn on power to the external VCC pin.
    // If Pin 13 is not driven LOW, the VCC pin remains unpowered (0V).
    pinMode(13, OUTPUT);
    digitalWrite(13, LOW); // LOW = Power ON
    delay(15); // Let power stabilize

    // Read and clear LATCH registers to see if we woke up from a button press
    uint32_t prevPinNum = g_ADigitalPinMap[PIN_BTN_PREV];
    uint32_t nextPinNum = g_ADigitalPinMap[PIN_BTN_NEXT];
    uint32_t selectPinNum = g_ADigitalPinMap[PIN_BTN_SELECT];

    auto checkAndClearLatch = [](uint32_t pinNum) -> bool {
        uint32_t port = pinNum >> 5;
        uint32_t pinMask = 1UL << (pinNum & 31);
        bool latched = false;
        if (port == 0) {
            if (NRF_P0->LATCH & pinMask) {
                latched = true;
                NRF_P0->LATCH = pinMask; // Clear latch bit by writing 1
            }
        } else {
            if (NRF_P1->LATCH & pinMask) {
                latched = true;
                NRF_P1->LATCH = pinMask; // Clear latch bit by writing 1
            }
        }
        return latched;
    };

    bool prevWakeup = checkAndClearLatch(prevPinNum);
    bool nextWakeup = checkAndClearLatch(nextPinNum);
    bool selectWakeup = checkAndClearLatch(selectPinNum);
    // Load isFlipped setting from settings file if it exists
    bool isFlipped = false;
    if (InternalFS.exists("/settings.dat")) {
        File file = InternalFS.open("/settings.dat", FILE_O_READ);
        if (file) {
            if (file.available()) file.readStringUntil('\n'); // skip FontType
            if (file.available()) file.readStringUntil('\n'); // skip FontSize
            if (file.available()) {
                String flipStr = file.readStringUntil('\n');
                flipStr.trim();
                if (flipStr.length() > 0) {
                    isFlipped = (flipStr.toInt() == 1);
                }
            }
            file.close();
        }
    }

    // If a button woke the board, inject a press and initialize the button's
    // state as already pressed to prevent double-clicking on release.
    if (prevWakeup) {
        Serial.println("[Button Debug] Wakeup triggered by PREV button.");
        if (isFlipped) {
            _nextBtn.event = BTN_CLICK;
        } else {
            _prevBtn.event = BTN_CLICK;
        }
        _prevBtn.lastPhysicalState = LOW;
        _prevBtn.debouncedState = LOW;
        _prevBtn.isLongPressed = true;
    }
    if (nextWakeup) {
        Serial.println("[Button Debug] Wakeup triggered by NEXT button.");
        if (isFlipped) {
            _prevBtn.event = BTN_CLICK;
        } else {
            _nextBtn.event = BTN_CLICK;
        }
        _nextBtn.lastPhysicalState = LOW;
        _nextBtn.debouncedState = LOW;
        _nextBtn.isLongPressed = true;
    }
    if (selectWakeup) {
        Serial.println("[Button Debug] Wakeup triggered by SELECT button.");
        _selectBtn.event = BTN_CLICK;
        _selectBtn.lastPhysicalState = LOW;
        _selectBtn.debouncedState = LOW;
        _selectBtn.isLongPressed = true;
    }

    // Configure button pins with internal pull-ups
    pinMode(PIN_BTN_PREV, INPUT_PULLUP);
    pinMode(PIN_BTN_NEXT, INPUT_PULLUP);
    pinMode(PIN_BTN_SELECT, INPUT_PULLUP);
}

void ButtonManager::_dummyISR() {
    // Intentional dummy ISR to wake up the MCU from sleep
}

void ButtonManager::enableWakeupInterrupts() {
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_PREV), _dummyISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_NEXT), _dummyISR, FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_BTN_SELECT), _dummyISR, FALLING);
}

void ButtonManager::disableWakeupInterrupts() {
    detachInterrupt(digitalPinToInterrupt(PIN_BTN_PREV));
    detachInterrupt(digitalPinToInterrupt(PIN_BTN_NEXT));
    detachInterrupt(digitalPinToInterrupt(PIN_BTN_SELECT));
}

void ButtonManager::setPeripheralPower(bool on) {
    // Active low on nice!nano: LOW = power ON, HIGH = power OFF
    // We only toggle this if dynamic VCC control is enabled or we want to power down during sleep
    digitalWrite(13, on ? LOW : HIGH);
    delay(5); // Let power stabilize
}

void ButtonManager::reset() {
    _prevBtn.event = BTN_NONE;
    _nextBtn.event = BTN_NONE;
    _selectBtn.event = BTN_NONE;
}

void ButtonManager::update() {
    uint32_t now = millis();
    ButtonState* buttons[] = {&_prevBtn, &_nextBtn, &_selectBtn};

    for (int i = 0; i < 3; i++) {
        ButtonState* btn = buttons[i];
        bool physicalState = digitalRead(btn->pin);

        // If state changed physically, reset debounce timer
        if (physicalState != btn->lastPhysicalState) {
            btn->lastDebounceTime = now;
            btn->lastPhysicalState = physicalState;
        }

        // If stable for DEBOUNCE_DELAY_MS
        if ((now - btn->lastDebounceTime) > DEBOUNCE_DELAY_MS) {
            if (physicalState != btn->debouncedState) {
                btn->debouncedState = physicalState;

                if (btn->debouncedState == LOW) {
                    // Button pressed (active low)
                    btn->pressStartTime = now;
                    btn->isLongPressed = false;
                } else {
                    // Button released (active high)
                    if (!btn->isLongPressed) {
                        ButtonState* targetBtn = btn;
                        if (DisplayManager::getInstance().isFlipped()) {
                            if (btn == &_prevBtn) targetBtn = &_nextBtn;
                            else if (btn == &_nextBtn) targetBtn = &_prevBtn;
                        }
                        targetBtn->event = BTN_CLICK;
                    }
                }
            }

            // Check for long press while button is held down
            if (btn->debouncedState == LOW && !btn->isLongPressed) {
                if ((now - btn->pressStartTime) > LONG_PRESS_DELAY_MS) {
                    ButtonState* targetBtn = btn;
                    if (DisplayManager::getInstance().isFlipped()) {
                        if (btn == &_prevBtn) targetBtn = &_nextBtn;
                        else if (btn == &_nextBtn) targetBtn = &_prevBtn;
                    }
                    targetBtn->event = BTN_LONG_PRESS;
                    btn->isLongPressed = true;
                }
            }
        }
    }
}

ButtonEvent ButtonManager::getPrevEvent() {
    ButtonEvent ev = _prevBtn.event;
    _prevBtn.event = BTN_NONE;
    return ev;
}

ButtonEvent ButtonManager::getNextEvent() {
    ButtonEvent ev = _nextBtn.event;
    _nextBtn.event = BTN_NONE;
    return ev;
}

ButtonEvent ButtonManager::getSelectEvent() {
    ButtonEvent ev = _selectBtn.event;
    _selectBtn.event = BTN_NONE;
    return ev;
}

void ButtonManager::injectPrevEvent(ButtonEvent ev) {
    _prevBtn.event = ev;
}

void ButtonManager::injectNextEvent(ButtonEvent ev) {
    _nextBtn.event = ev;
}

void ButtonManager::injectSelectEvent(ButtonEvent ev) {
    _selectBtn.event = ev;
}
