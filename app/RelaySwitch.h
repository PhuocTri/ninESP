/*
 * RelaySwitch.h
 *
 *  Created on: 30.11.2017
 *      Author: michael
 */

#include <SmingCore/SmingCore.h>

#ifndef APP_RELAYSWITCH_H_
#define APP_RELAYSWITCH_H_

class RelaySwitch {
public:
	RelaySwitch();
	void set(bool state);
	void toggle(void);
	bool get(void);
	void init(int8_t pin, bool defState);

protected:
	bool state;
private:
	int8_t pin;
};

#endif /* APP_RELAYSWITCH_H_ */
