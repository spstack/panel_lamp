#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pico/rand.h"
#include "hardware/clocks.h"
#include "hardware/uart.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/timer.h"


// UART defines
// By default the stdout UART is `uart0`, so we will use the second one
#define UART_ID uart1
#define BAUD_RATE 115200

// Use pins 4 and 5 for UART1
// Pins can be changed, see the GPIO function select table in the datasheet for information on GPIO assignments
#define UART_TX_PIN 4
#define UART_RX_PIN 5

#define LED_PIN 25

// LED Pin Defines
#define WHITE_LED_PIN 15
#define RED_LED_PIN 4
#define GREEN_LED_PIN 3
#define BLUE_LED_PIN 2
#define DEBUG_LED_PIN 16

// Button Pin Defines
#define BUTTON_BLACK_PIN 14 // Large black momentary push button
#define BUTTON_RED_PIN 13   // Large red button with locking mechanism

// Variable knob pin
#define KNOB_INPUT_PIN 28 // autotransformer output generally between 0-0.8VDC
#define KNOB_INPUT_ADC_CHANNEL 2 // ADC2 is on GPIO28
#define KNOB_POWER_CONTROL_PIN 12 // Pin to power the knob circuit

// Define voltage levels for min/max knob positions
#define KNOB_VOLTAGE_MAX_POS 800
#define KNOB_VOLTAGE_MIN_POS 50


// Task update periods
#define BUTTON_TASK_PERIOD_MS 25
#define LED_TASK_PERIOD_MS 20
#define KNOB_TASK_PERIOD_MS 100

// Set to 1 to enable debug output
#define DEBUG 1

// Error codes
typedef enum {
    NO_ERROR = 0,
    ERROR_GENERIC = -1,
    ERROR_BAD_PINNUM = -2,
    ERROR_NULL_POINTER = -3,

} error_code_e;


typedef enum {
    LED_MODE_OFF = 0,
    LED_MODE_WHITE_ONLY = 1,
    LED_MODE_WHITE_FLICKER = 2,
    LED_MODE_RGB_CYCLE = 3,
    LED_NUM_MODES = 4,
} led_mode_e;


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



error_code_e init_state(panel_state_t* state) {
    if (state == NULL) {
        return ERROR_NULL_POINTER;
    }

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
 * Initialize the PWM module on the provided pin
 */
error_code_e init_pwm_pin(int pinnum) {

    if (pinnum < 0 || pinnum > 29) {
        return ERROR_BAD_PINNUM;
    }

    // Set PWM pin to PWM function
    gpio_set_function(pinnum, GPIO_FUNC_PWM);

    // Get slice num
    uint slice_num = pwm_gpio_to_slice_num(pinnum);
    uint pwm_channel = pwm_gpio_to_channel(pinnum);

    pwm_set_clkdiv(slice_num, 8.0);
    pwm_set_wrap(slice_num, 500);
    pwm_set_chan_level(slice_num, pwm_channel, 250);

    pwm_set_enabled(slice_num, true);

    return NO_ERROR;
}

/**
 * Enable the output on the provided PWM pin
 */
void enable_pwm_output(int pinnum, bool enable) {
    uint slice_num = pwm_gpio_to_slice_num(pinnum);
    pwm_set_enabled(slice_num, enable);
}

/**
 * Set the duty cycle on the provided PWM pin in percent (0-100)
 */
void set_pwm_duty_cycle(int pinnum, int duty_cycle) {
    // Limit the duty cycle to 0-100%
    if (duty_cycle < 0) {
        duty_cycle = 0;
    } else if (duty_cycle > 100) {
        duty_cycle = 100;
    }

    // Convert percent to 0-500 range
    uint16_t chan_level_value = (duty_cycle * 500) / 100;

    uint slice_num = pwm_gpio_to_slice_num(pinnum);
    uint pwm_channel = pwm_gpio_to_channel(pinnum);
    pwm_set_chan_level(slice_num, pwm_channel, chan_level_value);
}


/**
 * Initialize LED pins as PWM outputs with 50% brightness
 */
void init_leds() {
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);

    gpio_init(DEBUG_LED_PIN);
    gpio_set_dir(DEBUG_LED_PIN, GPIO_OUT);
    gpio_put(DEBUG_LED_PIN, false);

    init_pwm_pin(WHITE_LED_PIN);
    set_pwm_duty_cycle(WHITE_LED_PIN, 50);

    init_pwm_pin(RED_LED_PIN);
    set_pwm_duty_cycle(RED_LED_PIN, 50);

    init_pwm_pin(GREEN_LED_PIN);
    set_pwm_duty_cycle(GREEN_LED_PIN, 50);

    init_pwm_pin(BLUE_LED_PIN);
    set_pwm_duty_cycle(BLUE_LED_PIN, 50);
}

/**
 * Initialize button GPIO pins
 */
void init_buttons() {
    // Initialize button GPIO pins
    gpio_init(BUTTON_BLACK_PIN);
    gpio_set_dir(BUTTON_BLACK_PIN, GPIO_IN);    // Set as input
    // gpio_pull_up(BUTTON_BLACK_PIN);        // Enable pull-up resistor

    gpio_init(BUTTON_RED_PIN);
    gpio_set_dir(BUTTON_RED_PIN, GPIO_IN);  // Set as input
    // gpio_pull_up(BUTTON_RED_PIN);          // Enable pull-up resistor
}

/**
 * Knob control pins
 * The knob measurement power control pin controls a PNP transistor that should be driven low to power the knob circuit
 * and set to high impedance (input) to turn off power to the knob circuit.
 */
void knob_power_off() {
    gpio_set_dir(KNOB_POWER_CONTROL_PIN, false); // Set as input to make high impedance (off state)
}
void knob_power_on() {
    gpio_set_dir(KNOB_POWER_CONTROL_PIN, true);  // Set as output
    gpio_put(KNOB_POWER_CONTROL_PIN, false); // Power on the knob circuit (active low)
}
void init_knob_control() {
    // Initialize knob power control pin
    gpio_init(KNOB_POWER_CONTROL_PIN);
    gpio_set_dir(KNOB_POWER_CONTROL_PIN, false); // Set as input to make high impedance (off state)

    // Initialize knob power on
    knob_power_on();
}


error_code_e init_adc(void) {
    // Initialize ADC hardware. Safe to call multiple times.
    adc_init();

    // ADC2 is on GPIO28
    adc_gpio_init(KNOB_INPUT_PIN);

    return NO_ERROR;
}

/**
 * Return true if the specified period (in ms) has elapsed since last_time
 */
bool has_time_elapsed(absolute_time_t last_time, uint32_t period_ms) {
    return absolute_time_diff_us(last_time, get_absolute_time()) >= (period_ms * 1000);
}

/* Return averaged raw ADC value (0..4095). `samples` must be >=1. */
uint16_t sample_knob_adc_input_raw(unsigned samples) {
    if (samples == 0) samples = 1;

    // Select the correct ADC input channel
    adc_select_input(KNOB_INPUT_ADC_CHANNEL);

    uint32_t sum = 0;
    for (unsigned i = 0; i < samples; ++i) {
        sum += adc_read();
    }

    // Average the value across multiple samples
    return (uint16_t)(sum / samples);
}

/* Return averaged ADC voltage in millivolts (approx). */
uint32_t sample_knob_adc_mv() {

    // Run 3 samples of the ADC and average them
    uint16_t raw = sample_knob_adc_input_raw(3);

    // Convert 0..4095 to mV assuming Vref ~ 3300 mV. Rounded.
    return (uint32_t)((raw * 3300u + 2047u) / 4095u);
}

/**
 * Task called from main loop to update button states
 */
void task_update_buttons(panel_state_t* state) {
    if (state == NULL) {
        return;
    }

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
    set_pwm_duty_cycle(WHITE_LED_PIN, state->white_led_brightness);
    set_pwm_duty_cycle(RED_LED_PIN, state->red_led_brightness);
    set_pwm_duty_cycle(GREEN_LED_PIN, state->green_led_brightness);
    set_pwm_duty_cycle(BLUE_LED_PIN, state->blue_led_brightness);

}

/**
 * Task to read knob position and update state
 */
void task_read_knob(panel_state_t* state) {
    if (state == NULL) {
        return;
    }

    // Make sure it's time to read the knob state!
    if (!has_time_elapsed(state->last_knob_time, KNOB_TASK_PERIOD_MS)) {
        return;
    }
    state->last_knob_time = get_absolute_time();

    // Read value of the knob position from ADC
    uint32_t knob_mv = sample_knob_adc_mv();
    state->knob_position_mv = knob_mv;
}

/**
 * Update state variables based on inputs (buttons, knob, etc.)
 */
void task_update_state(panel_state_t* state) {
    if (state == NULL) {
        return;
    }

    // Handle transition to next LED mode on black button press
    if (state->black_button_pressed_edge) {
        state->black_button_pressed_edge = false;
        printf("Black button pressed\n");

        // Go to the next state
        state->led_mode = (led_mode_e)((state->led_mode + 1) % LED_NUM_MODES);
        printf("LED mode changed to %d\n", state->led_mode);
    }

}

/**
 * Main entry point for the program
 */
int main()
{
    // Initialize the panel state
    panel_state_t panel_state;
    init_state(&panel_state);

    stdio_init_all();
    printf("System Clock Frequency is %d Hz\n", clock_get_hz(clk_sys));
    printf("USB Clock Frequency is %d Hz\n", clock_get_hz(clk_usb));

    // Set up our external UART
    uart_init(UART_ID, BAUD_RATE);
    gpio_set_function(UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(UART_RX_PIN, GPIO_FUNC_UART);

    // Initialize peripherals
    init_leds();
    init_buttons();
    init_adc();
    init_knob_control();

    // Send out a string, with CR/LF conversions
    // uart_puts(UART_ID, " Hello, UART!\n");
    printf("Initialization complete. Entering main loop.\n");

    // Main loop
    while (true) {

        // Update state based on inputs
        task_update_state(&panel_state);

        // Update low level hardware based on state
        task_update_buttons(&panel_state);
        task_read_knob(&panel_state);
        task_update_leds(&panel_state);
    }
}
