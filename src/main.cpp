//#define DEBUG

#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <AccelStepper.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <EEPROM.h>
#include <ESP32Encoder.h>
#include "MatrixDisplay.h"
#include "Timer.h"
#include "ButtonHandler.h"
#include "Settings.h"
#include "MatrixDisplay.h"

#define START_BUTTON_PIN 15   // Start button pin
#define HOMING_SWITCH_PIN 16  // Homing switch pin
#define ROTARY_CLK_PIN 17     // Rotary encoder CLK pin
#define ROTARY_DT_PIN 18      // Rotary encoder DT pin
#define ROTARY_SW_PIN 19      // Rotary encoder switch pin

// Initialize ButtonHandler objects
ButtonHandler buttonStart(START_BUTTON_PIN, "Start");
ButtonHandler buttonLimitSwitch(HOMING_SWITCH_PIN, "Limit", false);
ButtonHandler buttonRotarySwitch(ROTARY_SW_PIN, "Rotary");

// Initialize ESP32Encoder
ESP32Encoder encoder;

// Variables for encoder
int32_t lastEncoderValue = 0;
int32_t encoderValue = 0;

// Function to dump switch states and encoder value
static unsigned long lastDebugPrint = 0;
void dumpDebug() {
    unsigned long currentTime = millis();

    // Debug print every second
    if (currentTime - lastDebugPrint > 1000) {
        Serial.print("Start:");
        Serial.print(buttonStart.getState());
        Serial.print(" Limit:");
        Serial.print(buttonLimitSwitch.getState());
        Serial.print(" Rotary:");
        Serial.print(buttonRotarySwitch.getState());
        Serial.print(" Encoder:");
        Serial.print(encoderValue);
        Serial.print(" Direction:");
        Serial.println(encoderValue > lastEncoderValue ? "CW" : (encoderValue < lastEncoderValue ? "CCW" : "No change"));
        lastDebugPrint = currentTime;
    }
}

// Function to handle encoder changes
void handleEncoderChange(int32_t newValue) {
    #ifdef DEBUG
    if (newValue != lastEncoderValue) {
        Serial.print("Encoder ");
        Serial.print(newValue > lastEncoderValue ? "clockwise" : "anticlockwise");
        Serial.print(" to ");
        Serial.println(newValue);
    }
    #endif
    lastEncoderValue = newValue;
}

// Define pin connections
#define STEP_PIN 13
#define DIR_PIN 12
#define STEPPER_ENABLE_PIN 27  // Pin for Stepper Driver ENABLE
#define BUILTIN_LED_PIN 2  // Built-in LED pin for ESP32
#define ADDRESSABLE_LED_PIN 4  // New pin for Addressable LED
#define RELAY_PIN 14  // Relay control pin

// Define homing direction (1 for positive, -1 for negative)
#define HOMING_DIRECTION 1

// Define homing parameters
#define HOMING_DISTANCE 125.0 // Distance to move back after hitting the switch (in mm)
#define HOMING_SPEED 800.0 // Speed for homing movement
#define MOVE_TO_ZERO_SPEED 3000.0 // Speed for moving to zero position after homing

// Define system states
enum SystemState {
  STARTUP,
  HOMING,
  IDLE,
  RUNNING,
  RETURNING_TO_START,
  ERROR,
  SETTINGS_MENU
};

// Global variable to track system state
volatile SystemState currentSystemState = STARTUP;
SystemState previousSystemState = STARTUP;
bool stateJustChanged = true;

// Flag to control LCD update task
volatile bool lcdUpdateEnabled = false;

// Error message
String errorMessage = "";


// Timer variables
Timer timer;
unsigned long timerDuration = 30000; // 30 seconds, adjust as needed

// Define motor states
enum MotorState {
  MOVING,
  CHANGING_DIRECTION
};

// Movement and stepper motor parameters
const int STEPS_PER_REV = 1600;  // 200 * 8 (for 8 microstepping)
const float DISTANCE_PER_REV = 8.0;  // 8mm per revolution (lead of ACME rod)
float TOTAL_DISTANCE = 120.0;  // 30mm in each direction
int TOTAL_STEPS;
float MAX_SPEED = 1600;  // Maintains 2 revolutions per second (16 mm/second)
const float ACCELERATION = 3200.0;  // Adjust for smooth acceleration

// EEPROM addresses
const int EEPROM_TIMER_DURATION_ADDR = 0;
const int EEPROM_TOTAL_DISTANCE_ADDR = 4;
const int EEPROM_MAX_SPEED_ADDR = 8;

// Function to save parameters to EEPROM
void saveParametersToEEPROM() {
  EEPROM.put(EEPROM_TIMER_DURATION_ADDR, timerDuration);
  EEPROM.put(EEPROM_TOTAL_DISTANCE_ADDR, TOTAL_DISTANCE);
  EEPROM.put(EEPROM_MAX_SPEED_ADDR, MAX_SPEED);
  EEPROM.commit();
}

// Function to load parameters from EEPROM
void loadParametersFromEEPROM() {
  EEPROM.get(EEPROM_TIMER_DURATION_ADDR, timerDuration);
  EEPROM.get(EEPROM_TOTAL_DISTANCE_ADDR, TOTAL_DISTANCE);
  EEPROM.get(EEPROM_MAX_SPEED_ADDR, MAX_SPEED);
  
  // Check if values are valid (not NaN or infinity)
  if (isnan(TOTAL_DISTANCE) || isinf(TOTAL_DISTANCE)) TOTAL_DISTANCE = 120.0;
  if (isnan(MAX_SPEED) || isinf(MAX_SPEED)) MAX_SPEED = 1600;
  if (timerDuration == 0xFFFFFFFF) timerDuration = 30000; // Default value if EEPROM is empty
  
  // Update TOTAL_STEPS based on loaded TOTAL_DISTANCE
  TOTAL_STEPS = (TOTAL_DISTANCE / DISTANCE_PER_REV) * STEPS_PER_REV;
}

// Define LCD update interval
const unsigned long LCD_UPDATE_INTERVAL = 250;  // 0.25 second in milliseconds

// Initialize stepper
AccelStepper stepper(AccelStepper::DRIVER, STEP_PIN, DIR_PIN);

// Initialize MatrixDisplay
MatrixDisplay display(0x27, 16, 2);

// Initialize Settings
Settings settings(display, encoder);

// Variables for state machine
MotorState currentState = MOVING;
const unsigned long DIRECTION_CHANGE_DELAY = 500; // 500ms delay when changing direction

// Function to enter Settings menu
void enterSettingsMenu() {
  settings.enter();  // Enter settings menu
}

// Function to exit Settings menu
void exitSettingsMenu() {
  settings.exit();  // Exit settings menu
}

// Global variables for timing
unsigned long stateStartTime = 0;
const unsigned long WELCOME_DURATION = 1000;  // 5 seconds
const unsigned long HOMING_TIMEOUT = 30000;   // 30 seconds

// Function to convert SystemState to string
const char* getStateName(SystemState state) {
  switch(state) {
    case STARTUP: return "STARTUP";
    case HOMING: return "HOMING";
    case IDLE: return "IDLE";
    case RUNNING: return "RUNNING";
    case RETURNING_TO_START: return "RETURNING_TO_START";
    case ERROR: return "ERROR";
    case SETTINGS_MENU: return "SETTINGS_MENU";
    default: return "UNKNOWN";
  }
}

// New function to handle state changes
void changeState(SystemState newState, unsigned long currentTime = 0) {
  #ifdef DEBUG
  if(newState != previousSystemState){
    Serial.print("State changed from ");
    Serial.print(getStateName(previousSystemState));
    Serial.print(" to ");
    Serial.println(getStateName(newState));
  } else {
    Serial.print("Current state ");
    Serial.println(getStateName(newState));
  }
  #endif
  
  previousSystemState = currentSystemState;
  currentSystemState = newState;
  stateStartTime = currentTime == 0 ? millis() : currentTime;
  stateJustChanged = true;
}

void setup() {
  // Init if debug
  #ifdef DEBUG
  Serial.begin(115200);  // Initialize serial communication
  #endif

  // Initialize EEPROM
  EEPROM.begin(512);  // Initialize EEPROM with 512 bytes

  // Load parameters from EEPROM
  loadParametersFromEEPROM();

  // Initialize pins
  pinMode(BUILTIN_LED_PIN, OUTPUT);
  pinMode(ADDRESSABLE_LED_PIN, OUTPUT);
  pinMode(STEP_PIN, OUTPUT);
  pinMode(DIR_PIN, OUTPUT);
  pinMode(STEPPER_ENABLE_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);

  // Initialize ButtonHandler objects
  buttonStart.begin();
  buttonLimitSwitch.begin();
  buttonRotarySwitch.begin();

  // Initialize ESP32Encoder
  ESP32Encoder::useInternalWeakPullResistors=UP;
  encoder.attachHalfQuad(ROTARY_CLK_PIN, ROTARY_DT_PIN);
  encoder.setCount(0);

  // Set STEPPER_ENABLE_PIN to LOW to enable the stepper driver
  digitalWrite(STEPPER_ENABLE_PIN, LOW);

  // Initialize LCD
  display.begin();

  // Configure stepper
  stepper.setMaxSpeed(MAX_SPEED);
  stepper.setAcceleration(ACCELERATION);
  stepper.moveTo(0);  // Start at home position

  // Initialize and start MatrixDisplay update thread
  display.begin();
  display.startUpdateThread();

  // Initialize state
  changeState(STARTUP, millis());

  // Save parameters to EEPROM (in case they were not present)
  saveParametersToEEPROM();
}

void handleStartup(unsigned long currentTime) {
  if (stateJustChanged) {
    display.updateDisplay("OrangeMakers", "Marshmallow 2.0");
    stateJustChanged = false;
  }

  if (currentTime - stateStartTime >= WELCOME_DURATION) {
    changeState(HOMING, currentTime);
  }
}

static unsigned long lastHomingUpdateTime = 0;

void handleHoming(unsigned long currentTime) {
  static bool waitingForConfirmation = true;
  static long homingSteps = 0;
  static bool homingStarted = false;
  static bool movingAwayFromSwitch = false;

  if (stateJustChanged) {
    waitingForConfirmation = true;
    homingStarted = false;
    movingAwayFromSwitch = false;
    display.updateDisplay("To start homing", "press rotary");
    stateJustChanged = false;
    lastHomingUpdateTime = 0; // Reset the update time when state changes
  }

  if (waitingForConfirmation) {
    if (buttonRotarySwitch.isPressed()) {
      waitingForConfirmation = false;
      homingStarted = true;
      stateStartTime = currentTime;  // Reset the start time for homing
      stepper.setMaxSpeed(HOMING_SPEED);
      stepper.setAcceleration(ACCELERATION * 2);  // Set higher acceleration for more instant stop during homing
      homingSteps = HOMING_DIRECTION * 1000000;  // Large number to ensure continuous movement
      stepper.moveTo(homingSteps);
      display.updateDisplay("Homing:", "In progress");
    }
  } else if (homingStarted && !movingAwayFromSwitch) {
    if (buttonLimitSwitch.getState()) {
      display.updateDisplay("Homing:", "Triggered");
      stepper.stop();  // Stop as fast as possible: sets new target
      stepper.runToPosition();  // Wait for the stepper to stop
      
      // Wait for 1 second
      delay(1000);
      
      stepper.setMaxSpeed(MOVE_TO_ZERO_SPEED);
      stepper.setAcceleration(ACCELERATION);  // Restore original acceleration
      homingSteps = -HOMING_DIRECTION * (HOMING_DISTANCE / DISTANCE_PER_REV) * STEPS_PER_REV;  // Move HOMING_DISTANCE in opposite direction
      stepper.move(homingSteps);
      movingAwayFromSwitch = true;
    } else if (currentTime - stateStartTime > HOMING_TIMEOUT) {
      // Homing timeout
      errorMessage = "Homing failed";
      changeState(ERROR, currentTime);
      return;
    } else {
      stepper.run();
    }
  } else if (movingAwayFromSwitch) {
    if (stepper.distanceToGo() == 0) {
      // Finished moving away from switch
      stepper.setCurrentPosition(0);
      stepper.setMaxSpeed(MAX_SPEED);  // Restore original max speed
      display.updateDisplay("Homing:", "Completed");
      changeState(IDLE, currentTime);
    } else {
      stepper.run();
    }
  }
}

void handleIdle() {
  static unsigned long buttonPressStartTime = 0;
  const unsigned long LONG_PRESS_DURATION = 1000; // 1 second for long press

  if (stateJustChanged) {
    display.updateDisplay("Idle..", "Press Start");
    stateJustChanged = false;
  }

  if (buttonStart.isPressed()) {
    changeState(RUNNING, millis());
    timer.start(timerDuration);
    stepper.moveTo(-HOMING_DIRECTION * TOTAL_STEPS);  // Start moving in opposite direction of homing
    return;  // Exit the function immediately to start running
  }

  if (buttonRotarySwitch.getState()) {
    if (buttonPressStartTime == 0) {
      buttonPressStartTime = millis();
    } else if (millis() - buttonPressStartTime >= LONG_PRESS_DURATION) {
      changeState(SETTINGS_MENU, millis());
      enterSettingsMenu();
      buttonPressStartTime = 0;
      return;
    }
  } else {
    buttonPressStartTime = 0;
  }

  stepper.stop();
}

void handleRunning(unsigned long currentTime) {
  static unsigned long lastUpdateTime = 0;

  if (stateJustChanged) {
    stateJustChanged = false;
    display.updateDisplay("Cooking", "Started");
    timer.start(timerDuration);
    currentState = MOVING;  // Ensure we start in the MOVING state
    stepper.moveTo(-HOMING_DIRECTION * TOTAL_STEPS);  // Set initial movement direction
    lastUpdateTime = 0; // Force an immediate update
  }

  if (buttonStart.isPressed()) {
    changeState(RETURNING_TO_START, currentTime);
    display.updateDisplay("Cooking", "Aborted");
    stepper.moveTo(0);  // Set target to start position
    timer.stop();
    return;
  }

  if (timer.hasExpired()) {
    changeState(RETURNING_TO_START, currentTime);
    display.updateDisplay("Cooking", "Done");
    stepper.moveTo(0);  // Set target to start position
    timer.stop();
    return;
  }

  // Check if homing switch is triggered
  if (buttonLimitSwitch.getState()) {
    changeState(ERROR, currentTime);
    errorMessage = "Endstop trigger";
    return;
  }

  switch (currentState) {
    case MOVING:
      if (stepper.distanceToGo() == 0) {
        // Change direction when reaching either end
        stepper.moveTo(stepper.currentPosition() == 0 ? -HOMING_DIRECTION * TOTAL_STEPS : (stepper.currentPosition() == -HOMING_DIRECTION * TOTAL_STEPS ? 0 : -HOMING_DIRECTION * TOTAL_STEPS));
        digitalWrite(BUILTIN_LED_PIN, !digitalRead(BUILTIN_LED_PIN));  // Toggle LED when changing direction
        currentState = CHANGING_DIRECTION;
        stateStartTime = currentTime;
      } else {
        stepper.run();
      }
      break;

    case CHANGING_DIRECTION:
      if (currentTime - stateStartTime >= DIRECTION_CHANGE_DELAY) {
        currentState = MOVING;
      }
      break;
  }

  // Update LCD with remaining time and distance at specified interval
  if (currentTime - lastUpdateTime >= LCD_UPDATE_INTERVAL) {
    unsigned long remainingTime = timer.getRemainingTime() / 1000; // Convert to seconds
    float distance = abs(stepper.currentPosition() * DISTANCE_PER_REV / STEPS_PER_REV);
    
    String timeStr = "Time: " + String(remainingTime) + "s";
    String distStr = "Dist: " + String(distance, 1) + "mm";
    display.updateDisplay(timeStr, distStr);
    
    lastUpdateTime = currentTime;
  }
}

void handleReturningToStart() {
  static unsigned long lastUpdateTime = 0;
  unsigned long currentTime = millis();

  if (stateJustChanged) {
    stateJustChanged = false;
    lastUpdateTime = 0; // Force an immediate update
  }

  if (stepper.distanceToGo() == 0) {
    // We've reached the start position
    changeState(IDLE, currentTime);
    display.updateDisplay("Returned to", "Start Position");
  } else {
    stepper.run();

    if (currentTime - lastUpdateTime >= LCD_UPDATE_INTERVAL) {
      float distance = abs(stepper.currentPosition() * DISTANCE_PER_REV / STEPS_PER_REV);
      String returnStr = "Returning";
      String distStr = "Dist: " + String(distance, 1) + "mm";
      display.updateDisplay(returnStr, distStr);
      lastUpdateTime = currentTime;
    }
  }
}

void handleError() {
  if (stateJustChanged) {
    // Set STEPPER_ENABLE_PIN to HIGH to disable the stepper driver
    digitalWrite(STEPPER_ENABLE_PIN, HIGH);

    stateJustChanged = false;
  }
  
  // Always display the error message
  display.updateDisplay("Error", errorMessage);

  // In ERROR state, we don't do anything else until the device is reset
}

void loop() {
  unsigned long currentTime = millis();


  // Debug
  #ifdef DEBUG
  static unsigned long lastDebugPrint = 0;
  dumpDebug();
  #endif

  // Update all ButtonHandler objects
  buttonStart.update();
  buttonLimitSwitch.update();
  buttonRotarySwitch.update();

  // Read encoder value
  encoderValue = encoder.getCount();

  // Check if encoder value has changed
  if (encoderValue != lastEncoderValue) {
    // Handle encoder change
    int32_t change = encoderValue - lastEncoderValue;
    // You can use 'change' to update menu selection or modify values
    // For example: menuIndex = (menuIndex + change) % MENU_ITEMS;
    handleEncoderChange(encoderValue);
  }

  // Check for homing switch trigger in any state except HOMING, STARTUP, and ERROR
  if (currentSystemState != HOMING && currentSystemState != STARTUP && currentSystemState != ERROR && buttonLimitSwitch.getState()) {
    changeState(ERROR, currentTime);
    errorMessage = "Endstop trigger";
    handleError();  // Immediately handle the error
    return;  // Exit the loop to prevent further state processing
  }

  switch (currentSystemState) {
    case STARTUP:
      handleStartup(currentTime);
      break;
    case HOMING:
      handleHoming(currentTime);
      break;
    case IDLE:
      handleIdle();
      break;
    case RUNNING:
      handleRunning(currentTime);
      break;
    case RETURNING_TO_START:
      handleReturningToStart();
      break;
    case ERROR:
      handleError();
      break;
    case SETTINGS_MENU:
      settings.update();
      if (settings.isDone()) {
        exitSettingsMenu();
        changeState(IDLE, currentTime);
      }
      break;
  }

  // Reset changed states after handling
  buttonStart.reset();
  buttonLimitSwitch.reset();
  buttonRotarySwitch.reset();
}
