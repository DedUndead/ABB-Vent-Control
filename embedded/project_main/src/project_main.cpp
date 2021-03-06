/*
===============================================================================
 Name        : main.c
 Author      : Arefyev Pavel, Daniel Liberman, Alex Franko
 Version     : X
 License     : GPL-2.0 License
 Description : https://github.com/DedUndead/ABB-Vent-Control
===============================================================================
*/

#include "project_main.h"

extern "C" {
	void SysTick_Handler(void)
	{
		systicks++;
		if (delay > 0)  delay--;

		// Flags for polling
		if (systicks % VENT_TICK_T == 0) tick_ready = true;
		if (systicks % MQTT_UPDATE_TIMEOUT == 0) sample_ready = true;
	}

	void PIN_INT0_IRQHandler(void)
	{
		Chip_PININT_ClearIntStatus(LPC_GPIO_PIN_INT, PININTCH(0));

		// Button debounce
		if (millis() - last_pressed < DEBOUNCE) return;
		last_pressed = millis();

		menu_events_ptr->publish(MenuItem::up);
	}

	void PIN_INT1_IRQHandler(void)
	{
		Chip_PININT_ClearIntStatus(LPC_GPIO_PIN_INT, PININTCH(1));

		// Button debounce
		if (millis() - last_pressed < DEBOUNCE) return;
		last_pressed = millis();

		menu_events_ptr->publish(MenuItem::ok);
	}

	void PIN_INT2_IRQHandler(void)
	{
		Chip_PININT_ClearIntStatus(LPC_GPIO_PIN_INT, PININTCH(2));

		// Button debounce
		if (millis() - last_pressed < DEBOUNCE) return;
		last_pressed = millis();

		menu_events_ptr->publish(MenuItem::down);
	}
}

/* MAIN PROGRAM BODY */
int main(void) {

#if defined (__USE_LPCOPEN)
    SystemCoreClockUpdate();
#if !defined(NO_BOARD_LIB)
    Board_Init();
#endif
#endif

    /* Pressure sensor initialization */
    I2C i2c(I2C_CLOCKDIV, I2C_BITRATE, I2C_MODE);
    SdpSensor pressure_sensor(&i2c);

    /* Configure systick and RIT timers */
    Chip_RIT_Init(LPC_RITIMER);
    set_systick(SYSTICKRATE_HZ);

    /* Drive peripheral */
    AbbDrive abb_drive;

    /* Remove after debugging finished */
	LpcPinMap none = {-1, -1}; // unused pin has negative values in it
	LpcPinMap txpin = { 0, 18 }; // transmit pin that goes to debugger's UART->USB converter
	LpcPinMap rxpin = { 0, 13 }; // receive pin that goes to debugger's UART->USB converter
	LpcUartConfig cfg = { LPC_USART0, 115200, UART_CFG_DATALEN_8 | UART_CFG_PARITY_NONE | UART_CFG_STOPLEN_1, false, txpin, rxpin, none, none };
	LpcUart uart(cfg);

    /* Initialize LCD */
    DigitalIoPin rs(0, 10);
    DigitalIoPin en(0, 9);
    DigitalIoPin d4(1, 8);
    DigitalIoPin d5(0, 5);
    DigitalIoPin d6(0, 6);
    DigitalIoPin d7(0, 7);
    LiquidCrystal lcd(&rs, &en, &d4, &d5, &d6, &d7);

    /* Initialize menu */
    SimpleMenu menu;
    EventQueue menu_queue;

    std::string labels[MENU_LABELS_N] = { "Manual", "Automatic" };
    StringEdit mode(&lcd, std::string("Mode"), labels, MENU_LABELS_N);
    IntegerEdit freq(&lcd, std::string("Frequency"), FREQ_MAX, FREQ_MIN, FREQ_LCD_STEP, true);
    IntegerEdit target(&lcd, std::string("Target pressure"), PRES_MAX, PRES_MIN, PRES_STEP, false);
    IntegerEdit pressure(&lcd, std::string("Pressure"), PRES_MAX, PRES_MIN, PRES_STEP, false);

    menu.addItem(new MenuItem(&mode));
    menu.addItem(new MenuItem(&freq));
    menu.addItem(new MenuItem(&target));
    menu.addItem(new MenuItem(&pressure));

    menu_ptr = &menu;
    menu_events_ptr = &menu_queue;

    /* Configure menu values */
    mode.setValue(0);
    freq.setValue(0);
    target.setValue(0);
    pressure.setValue(0);

    /* Show initial menu values, create buffers */
    PropertyEdit* menu_items[MENU_ITEMS_NUM] = { &mode, &freq, &target, &pressure };
    menu.event(MenuItem::show);

    /* Menu buttons */
    DigitalIoPin up(1, 3, true, true, true);
    DigitalIoPin ok(0, 0, true, true, true);
    DigitalIoPin down(0, 24, true, true, true);

    delay_systick(INITIAL_DELAY); // Wait for the current to settle

    /* I/O interrupts initialization */
    DigitalIoPin::init_gpio_interrupts();
    up.enable_interrupt(0);
    ok.enable_interrupt(1);
    down.enable_interrupt(2);

    /* Configure MQTT */
    MQTT mqtt(mqtt_message_handler);
    mqtt.connect(NETWORK_SSID, NETWORK_PASS, MQTT_IP, MQTT_PORT);
    mqtt.subscribe(MQTT_TOPIC_RECEIVE);

    /* Initialize main state machine */
    SmartVent ventilation(&pressure_sensor, &abb_drive);

    /* Main polling loop */
    int sample_number = 0;
    int mqtt_status = 0;
    status status = ventilation.get_status();
    while (true) {
    	// Obtain event requests from LCD UI
    	while (menu_queue.pending()) menu.event(menu_queue.consume());
    	handle_lcd_input(&ventilation, menu_items);

    	// Send tick event
    	if (tick_ready) {
    		ventilation.handle_state(Event(Event::eTick));
    		status = ventilation.get_status(); // Update ventilation status
    		update_lcd(&ventilation, menu_items);

			tick_ready = false;
    	}

    	// Obtain sample and send over mqtt
    	if (sample_ready) {
    		std::string sample = get_sample_json(&ventilation, sample_number++);
    		mqtt_status = mqtt.publish(MQTT_TOPIC_SEND, sample, sample.length());
    		uart.write("MQTT status: " + std::to_string(mqtt_status) + "\r\n");

    		sample_ready = false;
    	}

    	// Obtain event requests from WEB UI
    	if (mqtt_message_arrived) {
    		uart.write(mqtt_message + "\r\n");
    		handle_mqtt_input(&ventilation, menu_items);
    	}

    	// MQTT Error handling. TODO: Reconnect
    	if (mqtt_status != MQTT_OK) {
    		mqtt.disconnect();
    		lcd.print("Error: MQTT");
    		lcd.print("Connection lost", LCD_NEXT_LINE);

    		ERROR_CONDITION();
    	}

    	// Component failure handling
    	if (status.operation_status == VENT_HARDWARE_ERROR) {
    		lcd.print("Error:          ");
    		lcd.print("Component error", LCD_NEXT_LINE);

    		delay_systick(ERROR_TIMEOUT);
    		menu.event(MenuItem::show);
    	}

    	// Setpoint timeout failure handling
    	if (status.operation_status == VENT_TIMEOUT_ERROR) {
    		lcd.print("Error:          ");
    		lcd.print("Setpoint timeout", LCD_NEXT_LINE);

    		delay_systick(ERROR_TIMEOUT);
    		menu.event(MenuItem::show);
    	}

    	mqtt_status = mqtt.yield(MQTT_YIELD_TIME);
    }

    return 0;
}

/* SUPPORT FUNCTIONS DEFINITIONS */

/**
 * @brief Construct sample data in JSON format
 * @param  ventilation   Pointer to target state machine
 * @param  sample_number Number of sample
 * @return JSON string
 */
std::string get_sample_json(SmartVent* ventilation, int sample_number)
{
	status status = ventilation->get_status();

	nlohmann::json sample;
	sample["nr"] = sample_number;
	sample["speed"] = status.frequency;
	sample["setpoint"] = status.target_pressure;
	sample["pressure"] = status.pressure;
	sample["mode"] = status.mode;
	sample["status"] = status.operation_status;

	return sample.dump();
}

/**
 * @brief Handle MQTT input, derive events
 * @param ventilation Pointer to target state machine
 * @param menu_items  Pointer to LCD's menu items to keep them updated with latest changes
 */
void handle_mqtt_input(SmartVent* ventilation, PropertyEdit* items[MENU_ITEMS_NUM])
{
	mqtt_message_arrived = false;

	// Get message in JSON structure and current machine status in status structure
	status current_status = ventilation->get_status();
	nlohmann::json settings = nlohmann::json::parse(mqtt_message);

	// Take necessary values from JSON data
	int new_settings[MENU_ITEMS_NUM - 1] = {
		settings.value("mode", current_status.mode),
		settings.value("speed", current_status.frequency),
		settings.value("setpoint", current_status.target_pressure),
	};

	// Derive new events from the settings
	for (int i = 0; i < MENU_ITEMS_NUM - 1; i++) {
		(*item_handlers[i])(ventilation, items, new_settings[i]); // Call handler
	}
}

/**
 * @brief Observe user inputs in LCD
 * @param ventilation Pointer to target state machine
 * @param menu_items  Pointer to LCD's menu items to handle inputs
 * If any present, call the respective handler
 */
void handle_lcd_input(SmartVent* ventilation, PropertyEdit* items[MENU_ITEMS_NUM])
{
	// Obtain current LCD items statuses
	for (int i = 0; i < MENU_ITEMS_NUM - 1; i++) {
		(*item_handlers[i])(ventilation, items, items[i]->getValue()); // Call handler
	}
}

/**
 * @brief Update LCD UI with current state machine status
 * @param ventilation Pointer to target state machine
 * @param menu_items  Pointer to LCD's menu items to handle inputs
 * If any present, call the respective handler
 */
void update_lcd(SmartVent* ventilation, PropertyEdit* items[MENU_ITEMS_NUM])
{
	// Get status
	status status = ventilation->get_status();

	// Obtain current state machine status
	int current_status[MENU_ITEMS_NUM] = {
		status.mode,
		status.frequency,
		status.target_pressure,
		status.pressure
	};

	for (int i = 0; i < MENU_ITEMS_NUM; i++) {
		(*item_handlers[i])(ventilation, items, current_status[i]); // Call handler
	}
}

/**
 * @brief Mode handler
 * @param ventilation Pointer to target state machine
 * @param menu_items  Pointer to LCD's menu items to keep them updated with latest changes
 * @param value       Target value
 * Universal handler for mqtt and lcd UIs input, lcd output
 */
void handle_mode(SmartVent* ventilation, PropertyEdit* menu_items[MENU_ITEMS_NUM], int value)
{
	// Update LCD menu if not being modified
	if (value != menu_items[iMode]->getValue() && !menu_items[iMode]->getFocus()) {
		menu_items[iMode]->setValue(value);
		menu_ptr->event(MenuItem::show);
	}

	// See if update in state machine needed
	if (value == ventilation->get_status().mode) return;

	// Send event to state machine
	if (value == SmartVent::mManual) {
		ventilation->handle_state(Event(Event::eManual));

		menu_items[iFrequency]->toggle_adjust();
		menu_items[iTarget]->toggle_adjust();
	}
	else {
		ventilation->handle_state(Event(Event::eAuto));

		menu_items[iFrequency]->toggle_adjust();
		menu_items[iTarget]->toggle_adjust();
	}
}

/**
 * @brief Frequency handler
 * @param ventilation Pointer to target state machine
 * @param menu_items  Pointer to LCD's menu items to keep them updated with latest changes
 * @param value       Target value
 * Universal handler for mqtt and lcd UIs input, lcd output  * Skip if mode is automatic
 */
void handle_freq(SmartVent* ventilation, PropertyEdit* menu_items[MENU_ITEMS_NUM], int freq)
{
	// Update LCD menu if not being modified
	if (freq != menu_items[iFrequency]->getValue() && !menu_items[iFrequency]->getFocus()) {
		menu_items[iFrequency]->setValue(freq);
		menu_ptr->event(MenuItem::show);
	}

	// See if update in state machine needed
	if (freq == ventilation->get_status().frequency) return;

	// Send event to state machine
	ventilation->handle_state(Event(Event::eFreq, freq));
}

/**
 * @brief Target pressure handler
 * @param ventilation Pointer to target state machine
 * @param menu_items  Pointer to LCD's menu items to keep them updated with latest changes
 * @param value       Target value
 * Universal handler for mqtt and lcd UIs input, lcd output
 */
void handle_target_pressure(SmartVent* ventilation, PropertyEdit* menu_items[MENU_ITEMS_NUM], int pressure)
{
	// Update LCD menu if not being modified
	if (pressure != menu_items[iTarget]->getValue() && !menu_items[iTarget]->getFocus()) {
		menu_items[iTarget]->setValue(pressure);
		menu_ptr->event(MenuItem::show);
	}

	// See if update in state machine needed
	if (pressure == ventilation->get_status().target_pressure) return;

	// Send event to state machine
	ventilation->handle_state(Event(Event::ePres, pressure));
}

/**
 * @brief Current pressure handler
 * @param ventilation Pointer to target state machine [Presence is necessary for handlers array]
 * @param menu_items  Pointer to LCD's menu items to keep them updated with latest changes
 * @param value       Target value
 * Universal handler for lcd output
 */
void handle_pressure(SmartVent* ventilation, PropertyEdit* menu_items[MENU_ITEMS_NUM], int pressure)
{
	// Update LCD menu
	if (pressure != menu_items[iPressure]->getValue()) {
		menu_items[iPressure]->setValue(pressure);
		menu_ptr->event(MenuItem::show);
	}
}

/**
 * @brief Configure systick to a specific frequency
 * @param freq Target frequency
 */
void set_systick(const int& freq)
{
    uint32_t sys_tick_rate;
    Chip_Clock_SetSysTickClockDiv(1);
    sys_tick_rate = Chip_Clock_GetSysTickClockRate();
    SysTick_Config(sys_tick_rate / freq);
}

/**
 * @brief Delay execution by amount of systicks
 * @param ticks Number of ticks for delay
 */
void delay_systick(const int ticks)
{
	delay = ticks;
	while (delay > 0) {
		__WFI();
	}
}

/**
 * @brief Return time elapsed from program start
 * @return Time in ms
 */
uint32_t millis()
{
	return systicks;
}

/**
 * @brief Handle message that aarrived via MQTT
 * Parse the message and store it in shared buffer
 * @param data Internal MQTT data typr
 */
void mqtt_message_handler(MessageData* data)
{
	mqtt_message_arrived = true;
	mqtt_message = "";

	// Parse message from payload
	char payload_parsed[READ_BUF_LENGTH];
	snprintf(
			payload_parsed,
			data->message->payloadlen + 1,
			"%.*s",
			data->message->payloadlen,
			(char *)data->message->payload
	);
	mqtt_message = payload_parsed;
}

void ERROR_CONDITION()
{
	while (true) delay_systick(100);
}
