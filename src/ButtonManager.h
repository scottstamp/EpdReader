#ifndef BUTTON_MANAGER_H
#define BUTTON_MANAGER_H

#include <Arduino.h>

enum ButtonEvent {
    BTN_NONE,
    BTN_CLICK,
    BTN_LONG_PRESS
};

class ButtonManager {
public:
    static ButtonManager& getInstance();

    void begin();
    void update();
    void reset();

    // Check if buttons were clicked
    ButtonEvent getPrevEvent();
    ButtonEvent getNextEvent();
    ButtonEvent getSelectEvent();

    // Event injection for button simulation (e.g. over BLE)
    void injectPrevEvent(ButtonEvent ev);
    void injectNextEvent(ButtonEvent ev);
    void injectSelectEvent(ButtonEvent ev);

    // Low power wake up configuration
    void enableWakeupInterrupts();
    void disableWakeupInterrupts();
    
    // nice!nano VCC power control
    void setPeripheralPower(bool on);

private:
    ButtonManager();
    ButtonManager(const ButtonManager&) = delete;
    ButtonManager& operator=(const ButtonManager&) = delete;

    struct ButtonState {
        int pin;
        bool lastPhysicalState;
        bool debouncedState;
        uint32_t lastDebounceTime;
        uint32_t pressStartTime;
        bool isLongPressed;
        ButtonEvent event;
    };

    ButtonState _prevBtn;
    ButtonState _nextBtn;
    ButtonState _selectBtn;

    static void _dummyISR();
};

#endif // BUTTON_MANAGER_H
