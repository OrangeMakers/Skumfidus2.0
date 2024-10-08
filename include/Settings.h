#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>
#include <ESP32Encoder.h>
#include <vector>
#include "ButtonHandler.h"
#include "MatrixDisplay.h"

extern ButtonHandler buttonRotarySwitch;

class Settings {
public:
    Settings(MatrixDisplay& display, ESP32Encoder& encoder);
    void loadSettingsFromPreferences();
    void saveSettingsToPreferences();
    unsigned long getCookTime() const;
    float getTotalDistance() const;
    float getSpeed() const;
    int getTotalSteps() const;
    static constexpr float DISTANCE_PER_REV = 8.0f;
    static constexpr int STEPS_PER_REV = 1600;
    ~Settings();

    void enter();
    void exit();
    void update();
    bool isDone() const;

private:
    int _totalSteps;
    MatrixDisplay& _display;
    enum class MenuItem {
        COOK_TIME,
        TOTAL_DISTANCE,
        MAX_SPEED,
        LOAD_EEPROM,
        SAVE_EEPROM,
        EXIT,
        FACTORY_RESET
    };

    struct MenuItemInfo {
        MenuItem item;
        const char* displayName;
        bool visible;
    };

    ESP32Encoder& _encoder;
    bool _isDone;
    bool _inEditMode;
    std::vector<MenuItemInfo> _menuItems;
    size_t _currentMenuIndex;
    int32_t _lastEncoderValue;

    bool _settingsChanged;
    unsigned long _initialCookTime;
    float _initialTotalDistance;
    float _initialSpeed;

    void initializeMenuItems();
    void updateMenuVisibility();
    void displayCurrentMenuItem();
    int8_t getEncoderDirection();
    void handleMenuNavigation(int8_t direction);
    void handleMenuSelection();
    void enterEditMode();
    void exitEditMode();
    void adjustValue(int8_t direction);
    void adjustCookTime(int8_t direction);
    void adjustTotalDistance(int8_t direction);
    void adjustMaxSpeed(int8_t direction);
    void updateDisplay();
    void factoryReset();
    bool confirmAction(const char* message);

    unsigned long _cookTime;
    float _totalDistance;
    float _speed;

    static constexpr float SPEED_MIN = 500.0f;
    static constexpr float SPEED_MAX = 3500.0f;
};

#endif // SETTINGS_H
