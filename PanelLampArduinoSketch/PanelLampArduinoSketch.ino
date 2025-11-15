/**
  Panel Lamp
  Code for the instrument panel lamp controller
*/

// Debug pin def
#define DEBUG_LED_PIN 13

// Button inputs
#define RED_BUTTON_PIN 10
#define BLACK_BUTTON_PIN 11

// LED Strip outputs
#define WHITE_LED_PIN 3
#define RED_LED_PIN 5
#define GREEN_LED_PIN 6
#define BLUE_LED_PIN 9

// Knob input pin
#define KNOB_IN_PIN A0
#define KNOB_POWER_CONTROL_PIN 2


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
    absolute_time_t last_button_time;
    uint8_t black_button_history;
    bool black_button_pressed;
    bool black_button_pressed_edge;
    bool black_button_released_edge;
    uint8_t red_button_history;
    bool red_button_pressed;
    bool red_button_pressed_edge;
    bool red_button_released_edge;

    absolute_time_t last_led_time;
    led_mode_e led_mode;
    uint8_t red_led_brightness;
    uint8_t green_led_brightness;
    uint8_t blue_led_brightness;
    uint8_t white_led_brightness;

    absolute_time_t last_knob_time;
    uint32_t knob_position_mv;

} panel_state_t;



// FUNCTION DECLARATIONS
void task_check_buttons(panel_state_t* panel_state);



// FUNCTION DEFINITIONS



/**
  Setup function run once at boot
*/
void setup() {
  
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
}

void init_state(panel_state_t* state) {

    // Initialize state variables
    state->last_button_time = get_absolute_time();
    state->black_button_history = 0;        // Active low button, so start with all zeros
    state->black_button_pressed = false;
    state->red_button_history = 0xFF;       // Active high button, so start with all ones
    state->red_button_pressed = false;

    // Init LED brightness levels
    state->last_led_time = get_absolute_time();
    state->led_mode = LED_MODE_OFF; // not used currently
    state->red_led_brightness = 0;
    state->green_led_brightness = 0;
    state->blue_led_brightness = 0;
    state->white_led_brightness = 50;

    state->knob_position_mv = 0;
    state->last_knob_time = get_absolute_time();

    return NO_ERROR;
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
 * Task called from main loop to update button states
 */
void task_update_buttons(panel_state_t* state) {

    // Make sure it's time to update the buttons
    if (!has_time_elapsed(state->last_button_time, BUTTON_TASK_PERIOD_MS)) {
        return;
    }
    state->last_button_time = get_absolute_time();

    // Toggle debug LED to indicate button task is running
    gpio_put(DEBUG_LED_PIN, !gpio_get(DEBUG_LED_PIN));

    // Clear edge detection flags
    state->black_button_pressed_edge = false;
    state->black_button_released_edge = false;
    state->red_button_pressed_edge = false;
    state->red_button_released_edge = false;

    // Update black button history
    state->black_button_history = (state->black_button_history << 1) | (gpio_get(BUTTON_BLACK_PIN) ? 1 : 0);

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
    state->red_button_history = (state->red_button_history << 1) | (gpio_get(BUTTON_RED_PIN) ? 1 : 0);

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

}


/**
 * Task called from main loop to update LED states based on panel state
 */
void task_update_leds(panel_state_t* state) {
    if (state == NULL) {
        return;
    }

    // Make sure it's time to update the LEDs
    if (!has_time_elapsed(state->last_led_time, LED_TASK_PERIOD_MS)) {
        return;
    }
    state->last_led_time = get_absolute_time();

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
                state->white_led_brightness = 70 + (get_rand_32() % 31);
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

}

/**
 * Update state variables based on inputs (buttons, knob, etc.)
 */
void task_update_state(panel_state_t* state) {

    // Handle transition to next LED mode on black button press
    if (state->black_button_pressed_edge) {
        state->black_button_pressed_edge = false;

        if (debug) {
          ("Black button pressed\n");  
        }
        

        // Go to the next state
        state->led_mode = (led_mode_e)((state->led_mode + 1) % LED_NUM_MODES);
        printf("LED mode changed to %d\n", state->led_mode);
    }

}

/**
  Main loop
*/
void loop() {
  panel_state_t state;

    // Update state based on inputs
  task_update_state(&panel_state);

  // Update low level hardware based on state
  task_update_buttons(&panel_state);
  task_read_knob(&panel_state);
  task_update_leds(&panel_state);



}
