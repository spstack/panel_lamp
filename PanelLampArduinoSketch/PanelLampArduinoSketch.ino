/**
  Panel Lamp
  Code for the instrument panel lamp controller
*/
#include <stdint.h>
#include <arduino-timer.h>

// debug flag
bool debug = false;

// CONSTANTS AND DEFINES

// Task periods (in ms)
#define BUTTON_TASK_PERIOD_MS 10
#define LED_TASK_PERIOD_MS 20
#define KNOB_TASK_PERIOD_MS 50

// Knob voltage range (in mV)
#define KNOB_VOLTAGE_MAX_POS 1400
#define KNOB_VOLTAGE_MIN_POS 15

// Debug pin def
#define DEBUG_LED_PIN 13

// Button inputs
#define RED_BUTTON_PIN 11
#define BLACK_BUTTON_PIN 10

// LED Strip outputs
#define WHITE_LED_PIN 3
#define RED_LED_PIN 5
#define GREEN_LED_PIN 6
#define BLUE_LED_PIN 9

// Knob input pin
#define KNOB_IN_PIN A0
#define KNOB_POWER_CONTROL_PIN 8


// STRUCTURES

typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_WHITE_ONLY = 1,
    LED_MODE_WHITE_FLICKER = 2,
    LED_MODE_RGB_CYCLE = 3,
    LED_NUM_MODES = 4,
} led_mode_e;

// Structure to hold all panel state data that can be
// passed to each sub-task
typedef struct {
    uint8_t black_button_history;
    bool black_button_pressed;
    bool black_button_pressed_edge;
    bool black_button_released_edge;
    uint8_t red_button_history;
    bool red_button_pressed;
    bool red_button_pressed_edge;
    bool red_button_released_edge;

    led_mode_e led_mode;
    uint8_t red_led_brightness;
    uint8_t green_led_brightness;
    uint8_t blue_led_brightness;
    uint8_t white_led_brightness;

    uint32_t knob_position_mv;

} panel_state_t;


// Global instantiation of panel state. This is ok because we are not using
// multi-threading or interrupts that would require re-entrancy.
panel_state_t panel_state;

// Global task scheduler/timer
// Allow up to 10 concurrent tasks, using ms as resolution, with handler argument of type panel_state_t*
// This allows us to schedule tasks with different periods easily
Timer<10, millis, panel_state_t*> TaskTimer;


// FUNCTION DECLARATIONS

// Tasks
bool task_update_buttons(panel_state_t* state);
bool task_update_leds(panel_state_t* state);
bool task_update_state(panel_state_t* state);
bool task_read_knob(panel_state_t* state);

// Utility functions
uint32_t sample_knob_adc_mv();
void knob_power_on();
void knob_power_off();


// FUNCTION DEFINITIONS

/**
  Setup function run once at boot
*/
void setup() {
    Serial.begin(115200);

    // Setup button pins
    pinMode(RED_BUTTON_PIN, INPUT);
    pinMode(BLACK_BUTTON_PIN, INPUT);

    // Setup LED PWM Outputs
    pinMode(WHITE_LED_PIN, OUTPUT);
    pinMode(RED_LED_PIN, OUTPUT);
    pinMode(GREEN_LED_PIN, OUTPUT);
    pinMode(BLUE_LED_PIN, OUTPUT);

    // Setup ADC inputs
    pinMode(KNOB_IN_PIN, INPUT);

    // Misc.
    pinMode(DEBUG_LED_PIN, OUTPUT);
    digitalWrite(DEBUG_LED_PIN, HIGH);

    // Power on the knob circuit by default
    knob_power_on();

    // Initialize panel state
    init_state(&panel_state);

    // Setup timer tasks
    TaskTimer.every(BUTTON_TASK_PERIOD_MS, task_update_buttons, &panel_state);
    TaskTimer.every(LED_TASK_PERIOD_MS, task_update_leds, &panel_state);
    TaskTimer.every(KNOB_TASK_PERIOD_MS, task_read_knob, &panel_state);
}

void init_state(panel_state_t* state) {

    // Initialize state variables
    state->black_button_history = 0;        // Active low button, so start with all zeros
    state->black_button_pressed = false;
    state->red_button_history = 0xFF;       // Active high button, so start with all ones
    state->red_button_pressed = false;

    // Init LED brightness levels
    state->led_mode = LED_MODE_OFF; // not used currently
    state->red_led_brightness = 0;
    state->green_led_brightness = 0;
    state->blue_led_brightness = 0;
    state->white_led_brightness = 50;

    state->knob_position_mv = 0;
}

/**
 * Knob control pins
 * The knob measurement power control pin controls a PNP transistor that should be driven low to power the knob circuit
 * and set to high impedance (input) to turn off power to the knob circuit.
 */

void knob_power_off() {
    pinMode(KNOB_POWER_CONTROL_PIN, INPUT); // Set as input to make high impedance (off state)
}
void knob_power_on() {
    pinMode(KNOB_POWER_CONTROL_PIN, OUTPUT);  // Set as output
    digitalWrite(KNOB_POWER_CONTROL_PIN, LOW); // Power on the knob circuit (active low)
}

/**
 * Sample the knob ADC and return the voltage in mV
 */
uint32_t sample_knob_adc_mv() {
    // Read ADC value and convert to mV
    uint32_t adc_value = (uint32_t)analogRead(KNOB_IN_PIN);

    // Assuming 10-bit ADC and 5V reference
    uint32_t voltage_mv = (adc_value * 5000) / 1023;

    return voltage_mv;
}

/**
 * Task to read knob position and update state
 */
bool task_read_knob(panel_state_t* state) {
    // Read value of the knob position from ADC
    uint32_t knob_mv = sample_knob_adc_mv();
    state->knob_position_mv = knob_mv;

    if (debug) {
      Serial.println(knob_mv);
    }
    return true;
}

/**
 * Task called from main loop to update button states
 */
bool task_update_buttons(panel_state_t* state) {

    // Clear edge detection flags
    state->black_button_pressed_edge = false;
    state->black_button_released_edge = false;
    state->red_button_pressed_edge = false;
    state->red_button_released_edge = false;

    // Update black button history
    state->black_button_history = (state->black_button_history << 1) | (digitalRead(BLACK_BUTTON_PIN) ? 1 : 0);

    // Check for press (assuming active low)
    if ((state->black_button_history & 0x0F) == 0x03) {
        state->black_button_released_edge = true;
    } else if ((state->black_button_history & 0x0F) == 0x0C) {
        state->black_button_pressed_edge = true;
    } else if ((state->black_button_history & 0x0F) == 0x00) {
        state->black_button_pressed = true;
    } else {
        state->black_button_pressed = false;
    }

    // Update red button history
    state->red_button_history = (state->red_button_history << 1) | (digitalRead(RED_BUTTON_PIN) ? 1 : 0);

    // Check for press (active high button)
    if ((state->red_button_history & 0x0F) == 0x0C) {
        state->red_button_released_edge = true;
    } else if ((state->red_button_history & 0x0F) == 0x03) {
        state->red_button_pressed_edge = true;
    } else if ((state->red_button_history & 0x0F) == 0x0F) {
        state->red_button_pressed = true;
    } else {
        state->red_button_pressed = false;
    }

    return true;
}


/**
 * Task called from main loop to update LED states based on panel state
 */
bool task_update_leds(panel_state_t* state) {

    uint8_t brightness = (state->knob_position_mv - KNOB_VOLTAGE_MIN_POS) * 100 / (KNOB_VOLTAGE_MAX_POS - KNOB_VOLTAGE_MIN_POS);
    if (state->knob_position_mv >= KNOB_VOLTAGE_MAX_POS) {
        brightness = 100;
    } else if (state->knob_position_mv <= KNOB_VOLTAGE_MIN_POS) {
        brightness = 0;
    }

    // If red button is not pressed, turn off all LEDs
    if (!state->red_button_pressed) {
        state->white_led_brightness = 0;
        state->red_led_brightness = 0;
        state->green_led_brightness = 0;
        state->blue_led_brightness = 0;

    } else {

        // Depending on mode, set LED brightness levels
        switch (state->led_mode) {
            case LED_MODE_OFF:
                state->white_led_brightness = 0;
                state->red_led_brightness = 0;
                state->green_led_brightness = 0;
                state->blue_led_brightness = 0;
                break;

            case LED_MODE_WHITE_ONLY:
                state->white_led_brightness = brightness;
                state->red_led_brightness = 0;
                state->green_led_brightness = 0;
                state->blue_led_brightness = 0;
                break;

            case LED_MODE_WHITE_FLICKER:
                // Simulate flicker by random brightness between 70-100%
                state->white_led_brightness = 70 + (analogRead(A1) % 31);
                state->red_led_brightness = 0;
                state->green_led_brightness = 0;
                state->blue_led_brightness = 0;
                break;

            case LED_MODE_RGB_CYCLE:
                // Cycle through RGB colors
                state->red_led_brightness = brightness;
                state->green_led_brightness = brightness;
                state->blue_led_brightness = brightness;
                state->white_led_brightness = brightness;
                break;

            default:
                // Should not happen
                state->white_led_brightness = 0;
                state->red_led_brightness = 0;
                state->green_led_brightness = 0;
                state->blue_led_brightness = 0;
                break;
        }
    }

    // Actually set the PWM duty cycles
    analogWrite(WHITE_LED_PIN, state->white_led_brightness);
    analogWrite(RED_LED_PIN, state->red_led_brightness);
    analogWrite(GREEN_LED_PIN, state->green_led_brightness);
    analogWrite(BLUE_LED_PIN, state->blue_led_brightness);

    return true;
}

/**
 * Update state variables based on inputs (buttons, knob, etc.)
 */
bool task_update_state(panel_state_t* state) {

    // Handle transition to next LED mode on black button press
    if (state->black_button_pressed_edge) {
        state->black_button_pressed_edge = false;

        // Go to the next state
        state->led_mode = (led_mode_e)((state->led_mode + 1) % LED_NUM_MODES);

        if (debug) {
            Serial.print("LED mode changed to ");
            Serial.println(state->led_mode);
        }
    }

    return true;
}

/**
  Main loop
*/
void loop() {
    // Update scheduled tasks
    TaskTimer.tick();

    // Update state based on inputs every loop iteration
    task_update_state(&panel_state);
}
