/*
 * RelaySwitch.cpp
 *
 *  Created on: 30.11.2017
 *      Author: Michael Sauer
 */

#include "RelaySwitch.h"
#include <SmingCore/SmingCore.h>

RelaySwitch::RelaySwitch()
{
	state = false;
	pin = -1;
}

void RelaySwitch::init(int8_t pin, bool inverted = false, bool defState = false)
{
	pinMode(pin, OUTPUT);
	this->pin = pin;
	this->inverted = inverted;
	if (inverted)
		digitalWrite(pin, !defState);
	else
		digitalWrite(pin, defState);
}

bool RelaySwitch::get(void)
{
	return state;
}

void RelaySwitch::set(bool state)
{
	if (state) {
		debugf("Relay ON");
		state = true;
		if (inverted)
			digitalWrite(pin, 0);
		else
			digitalWrite(pin, 1);
	} else {
		debugf("Relay OFF");
		state = false;
		if (inverted)
			digitalWrite(pin, 1);
		else
			digitalWrite(pin, 0);
	}
}

void RelaySwitch::toggle(void)
{
	digitalWrite(pin, !state);
	//mqtt.publish(AppSettings.relay_topic_pub, "on", true);
}

