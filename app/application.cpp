#include <user_config.h>
#include <app_config.h>
#include <SmingCore/SmingCore.h>
#include <AppSettings.h>
#include <Libraries/RCSwitch/RCSwitch.h>
#include <RelaySwitch.h>
#include <ninMQTTClient.h>
#include <LedMatrix.h>

//#define printf_P_stack

/* Web and FTP Server instance */
HttpServer server;
FTPServer ftp;

/* Contains list of available wifi networks after populating */
BssList networks;
/* Current network and password for wifi connection, used in ajax callback */
String network, password;

/* Last modified string for webinterface */
String lastModified;

/* Forward declarations */
void startMqttClient();
void onMQTTMessageReceived(String topic, String message);
void connectOk(IPAddress ip, IPAddress mask, IPAddress gateway);
void connectFail(String ssid, uint8_t ssidLength, uint8_t *bssid, uint8_t reason);
void onReceiveUDP(UdpConnection& connection, char *data, int size, IPAddress remoteIP, uint16_t remotePort);
void statusLed(bool state);
void motionSensorCheck();

/* UDP Connection instance */
UdpConnection udp = UdpConnection(onReceiveUDP);

/* MQTT client instance */
/* For quickly check you can use: http://www.hivemq.com/demos/websocket-client/ (Connection= test.mosquitto.org:8080) */
ninMqttClient mqtt(DEFAULT_MQTT_SERVER, DEFAULT_MQTT_PORT, false, onMQTTMessageReceived);

/* RelaySwitch and RCSwitch instance */
RCSwitch rcSwitch = RCSwitch();
RelaySwitch relay = RelaySwitch();

LedMatrix led = LedMatrix(DEFAULT_MAX7219_COUNT, DEFAULT_MAX7219_SS_PIN);

Timer displayTimer;

/* Timer for make connection ajax callback */
Timer connectionTimer;

/* Timer for publishing sensor values */
Timer sensorPublishTimer;

/* Timer to check for connection */
Timer checkConnectionTimer;

/* Timer to check for motion on motion sensor */
Timer motionCheckTimer;
bool motionState = false;

/* IRQ Callback for interrupt of key input
void IRAM_ATTR keyIRQHandler()
{
	if (AppSettings.relay) {
		relay.toggle();
		if (mqtt.getConnectionState() != eTCS_Connected) //TODO
			startMqttClient(); // Auto reconnect
		mqtt.publish(AppSettings.relay_topic_pub, String(relay.get()), true);
	}
}
*/

/* Callback for UDP Receiving */
void onReceiveUDP(UdpConnection& connection, char *data, int size, IPAddress remoteIP, uint16_t remotePort)
{
	debugf("UDP Sever callback from %s:%d, %d bytes", remoteIP.toString().c_str(), remotePort, size);

	/* Debug for RCSwitch */
	String d1,d2;
	String d = String(data);
	if ((d.charAt(0) == 'S' || d.charAt(0) == 'O' ) && d.length() == 12) {
		for (uint8_t i = 1; i <= 5; i++) {
			d1 += d.charAt(i);
			d2 += d.charAt(i+5);
		}
		if (d.charAt(0) == 'S') {
			rcSwitch.switchOn(d1.c_str(), d2.c_str());
			debugf("ON D1 = %s | D2 = %s", d1.c_str(), d2.c_str());
		} else if (d.charAt(0) == 'O') {
			rcSwitch.switchOff(d1.c_str(), d2.c_str());
			debugf("OFF D1 = %s | D2 = %s", d1.c_str(), d2.c_str());
		}
	} else if ((d.charAt(0) == 'P') && d.length() == 3) {
		String protocol = String(d.charAt(1));
		debugf("Set Protocol %s", protocol.c_str());
		rcSwitch.setProtocol(protocol.toInt());
	} else if ((d.charAt(0) == 'L')) {
		String pulselength;
		for (uint8_t i = 1; i < d.length(); i++) {
			pulselength += d.charAt(i);
		}
		debugf("Set Pulselength %s", pulselength.c_str());
		rcSwitch.setPulseLength(pulselength.toInt());
	}
	/* Debug RCSwitch End */

	// Send echo to remote sender
	String text = String("echo: ") + data;
	udp.sendStringTo(remoteIP, remotePort, text);
}

/* Callback for messages, arrived from MQTT server */
void onMQTTMessageReceived(String topic, String message)
{
	debugf("Debug: %s - %s", topic.c_str(), message.c_str());

	/* If Relay is enabled and topic equals the topic from config then check payload */
	if (AppSettings.relay && AppSettings.relay_topic_cmd.equals(topic)) {
		debugf("Relay Command received!");
		/* Check payload for valid message */
		if (message.equals("on") || message.equals("1")) { // Relay ON
			/* Set relay to on */
			relay.set(true);
			/* Check mqtt connection status */
			if (mqtt.getConnectionState() != eTCS_Connected)
				startMqttClient(); // Auto reconnect

			/* Publish current relay state */
			mqtt.publish(AppSettings.relay_topic_pub, "1", true);
		} else if (message.equals("off") || message.equals("0")) { // Relay OFF
			/* Set relay to off */
			relay.set(false);
			/* Check mqtt connection status */
			if (mqtt.getConnectionState() != eTCS_Connected)
				startMqttClient(); // Auto reconnect

			/* Publish current relay state */
			//TODO problem?
			mqtt.publish(AppSettings.relay_topic_pub, "0", true);
		}
	}

	/* RC Switch relay */
	if (AppSettings.rcswitch && (AppSettings.rcswitch_count > 0)) {
		for (uint8_t c = 0; c < AppSettings.rcswitch_count; c++) {
			Vector<String> dev = AppSettings.rcswitch_dev.get(c);
			String m0 = dev.get(0);
			String m1 = dev.get(1);
			if (topic.equals(AppSettings.rcswitch_topic_prefix + String(c))) {
				if (message.equals("on") || message.equals("1")) { // Relay ON
					if (m0.length() == 5)
						rcSwitch.switchOn(m0.c_str(), m1.c_str());
					else
						rcSwitch.switchOn((int)m0.toInt(), (int)m1.toInt());
				} else if (message.equals("off") || message.equals("0")) { // Relay OFF
					if (m0.length() == 5)
						rcSwitch.switchOff(m0.c_str(), m1.c_str());
					else
						rcSwitch.switchOff((int)m0.toInt(), (int)m1.toInt());
				}
			}
		}
	}
	/* RCSwitch relay end */
}

String getStatusString() {
	String status;
	if (WifiAccessPoint.isEnabled())
		status += "Access Point ";
	if (WifiStation.isEnabled()) {
		if (WifiAccessPoint.isEnabled())
			status += "& ";
		status += "Station ";
	}
	status += "Mode - ";
	if (WifiStation.isEnabled() && WifiStation.isConnected()) {
		IPAddress ip = WifiStation.getIP();
		String ssid = WifiStation.getSSID();
		status += "Connected to " + ssid + " with IP " + ip.toString();
	}
	if (WifiAccessPoint.isEnabled()) {
		IPAddress ip = WifiAccessPoint.getIP();
		String ssid = WifiAccessPoint.getSSID();
		if (WifiStation.isEnabled())
			status += " & ";
		status += "Wifi AP '" + ssid + "' on IP " + ip.toString();
	}
	return status;
}

/* Timer callback function to publish values from attached sensors */
void sensorPublish()
{
	/* If ADC is enabled */
	if (AppSettings.adc) {
		/* Read out ADC value */
		int a = system_adc_read();
		/* If publishing of ADC Values is enabled, then publish */
		if (AppSettings.adc_pub) {
			/* Check mqtt connection status */
			if (mqtt.getConnectionState() != eTCS_Connected)
				startMqttClient(); // Auto reconnect

			/* Publish mqtt adc value */
			mqtt.publish(AppSettings.adc_topic, String(a), true);
			debugf("sensorPublish() ADC published");
		}
	}
}

/* Start MQTT client and publish/subscribe to the used services */
void startMqttClient()
{
	if (mqtt.isEnabled()) {

		/* Set LWT message and topic */
		if(!mqtt.setWill(AppSettings.mqtt_topic_lwt, "offline", 2, true)) {
			debugf("Unable to set the last will and testament. Most probably there is not enough memory on the device.");
		} else {
			debugf("MQTT Last-Will and testament set");
		}

		/* Populate MQTT Settings from configuration */
		mqtt.setHost(AppSettings.mqtt_server);
		mqtt.setPort(AppSettings.mqtt_port);
		mqtt.connect(AppSettings.mqtt_userid, AppSettings.mqtt_login, AppSettings.mqtt_password);

		/* Subscribe to client command topic for general commands */
		mqtt.subscribe(AppSettings.mqtt_topic_cmd);

		/* Publish LWT message */
		mqtt.publishWithQoS(AppSettings.mqtt_topic_lwt, "online", 1, true);

		/* If relay is attached and enabled in settings */
		if (AppSettings.relay) {
			/* Publish current relay state to mqtt */
			mqtt.publish(AppSettings.relay_topic_pub, String(relay.get()), true);
			//TODO: Add relay config key, key_pin, relay_pin, ...
			/* Attach interrupt if enabled in config */
			//attachInterrupt(KEY_INPUT_PIN, keyIRQHandler, CHANGE);
			/* Check if set topics are at least 1 byte long */
			if ((AppSettings.relay_topic_cmd.length() >= 1) && (AppSettings.relay_topic_pub.length() >= 1)) {
				/* Check if topic backup is set */
				//if (AppSettings.relay_topic_cmd_old.length() > 0)
				//	mqtt.unsubscribe(AppSettings.relay_topic_cmd_old); /* Unsubscribe backup relay topic, for changes in webinterface */
				mqtt.subscribe(AppSettings.relay_topic_cmd); /* Resubscribe relay topic */
				/* Set backup relay topic to current relay topic */
				AppSettings.relay_topic_cmd_old = AppSettings.relay_topic_cmd;
			} else {
				// TODO: show error for to short relay topics with ajax
			}
		//} else {
			/* detach Interrupt pin if enabled in config */
			//detachInterrupt(KEY_INPUT_PIN);
			/* Relay is not enabled in config, check if there is a topic set and unsubscribe if so */
			/*if (AppSettings.relay_topic_cmd.length() >= 1) {
				mqtt.unsubscribe(AppSettings.relay_topic_cmd_old);
			}*/
		}

		if (AppSettings.rcswitch) {
			//TODO: Check if topic prefix got already trailing / and/or #
			mqtt.subscribe(AppSettings.rcswitch_topic_prefix + "#");
			rcSwitch.enableTransmit(AppSettings.rcswitch_pin);
		} else {
			//mqtt.unsubscribe(AppSettings.rcswitch_topic_prefix + "#");
			rcSwitch.disableTransmit();
		}

		if (AppSettings.max7219) {
			mqtt.subscribe(AppSettings.max7219_topic_prefix + AppSettings.max7219_topic_enable);
			mqtt.subscribe(AppSettings.max7219_topic_prefix + AppSettings.max7219_topic_text);
			mqtt.subscribe(AppSettings.max7219_topic_prefix + AppSettings.max7219_topic_charwidth);
			mqtt.subscribe(AppSettings.max7219_topic_prefix + AppSettings.max7219_topic_speed);
			mqtt.subscribe(AppSettings.max7219_topic_prefix + AppSettings.max7219_topic_scroll);
			mqtt.subscribe(AppSettings.max7219_topic_prefix + AppSettings.max7219_topic_intensity);
			//mqtt.subscribe("client2/display/oscil");
		}
	}
}

/* Start used services for connected peripherals */
void startServices()
{
	if (AppSettings.udp) {
		/* Close any udp connections */
		udp.close();
		/* Start UDP Server */
		if (AppSettings.udp_port > 0)
			udp.listen(AppSettings.udp_port);
		//TODO UDPserver: else error wrong/no udp port assigned
	}
	//attachInterrupt(16, sensorIRQHandler, CHANGE);
}

void checkMQTTConnection()
{
	/* Check if MQTT Connection is still alive */
	if (mqtt.getConnectionState() != eTCS_Connected)
		startMqttClient(); /* Auto reconnect */
}

/* Index target for webserver */
void onIndex(HttpRequest &request, HttpResponse &response)
{
	TemplateFileStream *tmpl = new TemplateFileStream("index.html");
	auto &vars = tmpl->variables();
	vars["lastedit"] = lastModified;

	vars["status"] = getStatusString();

	response.sendTemplate(tmpl); // will be automatically deleted
}

/* On Reboot target for webserver */
void onReboot(HttpRequest &request, HttpResponse &response)
{
	debugf("Restarting...");
	/* Disable Watchdog */
	//WDT.enable(true);
	/* Restart System */
	System.restart();
}

/* Network config target for webserver */
void onConfig(HttpRequest &request, HttpResponse &response)
{
	/* If values are passed from browser we will set here */
	if (request.method == HTTP_POST)
	{
		AppSettings.status_led_pin = request.getPostParameter("statuspin").toInt();
		AppSettings.status_led_inv = request.getPostParameter("statusinv").equals("on") ? true : false;

		AppSettings.dhcp = request.getPostParameter("dhcp") == "1";
		AppSettings.ip = request.getPostParameter("ip");
		AppSettings.netmask = request.getPostParameter("netmask");
		AppSettings.gateway = request.getPostParameter("gateway");

		AppSettings.udp = request.getPostParameter("udp").equals("on") ? true : false;;
		AppSettings.udp_port = request.getPostParameter("udpport").toInt();

		debugf("Updating IP settings: %d", AppSettings.ip.isNull());
		AppSettings.saveNetwork();
	}

	AppSettings.loadNetwork();

	TemplateFileStream *tmpl = new TemplateFileStream("settings.html");
	auto &vars = tmpl->variables();

	vars["statuspin"] = String(AppSettings.status_led_pin);
	vars["statusinv"] = AppSettings.status_led_inv ? "checked='checked'" : "";

	bool dhcp = WifiStation.isEnabledDHCP();
	vars["dhcpon"] = dhcp ? "checked='checked'" : "";
	vars["dhcpoff"] = !dhcp ? "checked='checked'" : "";

	vars["udpon"] = AppSettings.udp ? "checked='checked'" : "";
	vars["udpport"] = String(AppSettings.udp_port);

	if (!WifiStation.getIP().isNull())
	{
		vars["ip"] = WifiStation.getIP().toString();
		vars["netmask"] = WifiStation.getNetworkMask().toString();
		vars["gateway"] = WifiStation.getNetworkGateway().toString();
	}
	else
	{
		vars["ip"] = "192.168.1.77";
		vars["netmask"] = "255.255.255.0";
		vars["gateway"] = "192.168.1.1";
	}

	vars["lastedit"] = lastModified;

	vars["status"] = getStatusString();

	response.sendTemplate(tmpl); // will be automatically deleted
}

/* Peripheral config target for webserver */
void onPeriphConfig(HttpRequest &request, HttpResponse &response)
{
	/* If values are passed from browser we will set here */
	if (request.method == HTTP_POST)
	{
		AppSettings.timer_delay = String(request.getPostParameter("timer_delay")).toInt();

		/* Relay Settings */
		AppSettings.relay = request.getPostParameter("relay").equals("on") ? true : false;
		AppSettings.relay_pin = String(request.getPostParameter("relay_pin")).toInt();
		AppSettings.relay_invert = request.getPostParameter("relay_invert").equals("on") ? true : false;
		AppSettings.relay_topic_pub = request.getPostParameter("topic_relay_pub");
		AppSettings.relay_topic_cmd = request.getPostParameter("topic_relay_cmd");
		AppSettings.keyinput = request.getPostParameter("keyinput_on").equals("on") ? true : false;
		AppSettings.keyinput_invert = request.getPostParameter("keyinput_invert").equals("on") ? true : false;
		AppSettings.keyinput_pin = String(request.getPostParameter("keyinput_pin")).toInt();

		/* ADC Settings */
		AppSettings.adc = request.getPostParameter("adc").equals("on") ? true : false;
		AppSettings.adc_pub = request.getPostParameter("sensor_adc_pub").equals("1") ? true : false;
		AppSettings.adc_topic = request.getPostParameter("topic_adc_pub");

		/* RCSwitch Settings */
		AppSettings.rcswitch = request.getPostParameter("rcswitch").equals("on") ? true : false;
		AppSettings.rcswitch_topic_prefix = request.getPostParameter("topic_rcswitch");
		AppSettings.rcswitch_pin = String(request.getPostParameter("rcswitch_pin")).toInt();
		//TODO RCSWitch Save devices aswell

		/* MAX7219 Display Settings */
		AppSettings.max7219 = request.getPostParameter("max7219").equals("on") ? true : false;
		AppSettings.max7219_count = String(request.getPostParameter("max7219_count")).toInt();
		AppSettings.max7219_ss_pin = String(request.getPostParameter("max7219_pin")).toInt();
		AppSettings.max7219_topic_prefix = request.getPostParameter("max7219_prefix");
		AppSettings.max7219_topic_enable = request.getPostParameter("max7219_enable");
		AppSettings.max7219_topic_text = request.getPostParameter("max7219_text");
		AppSettings.max7219_topic_scroll = request.getPostParameter("max7219_scroll");
		AppSettings.max7219_topic_speed = request.getPostParameter("max7219_speed");
		AppSettings.max7219_topic_charwidth = request.getPostParameter("max7219_char");
		AppSettings.max7219_topic_intensity = request.getPostParameter("max7219_int");

		/* Motion Sensor Settings */
		AppSettings.motion = request.getPostParameter("motion").equals("on") ? true : false;
		AppSettings.motion_invert = request.getPostParameter("motion_invert").equals("on") ? true : false;
		AppSettings.motion_pin = request.getPostParameter("motion_pin").toInt();
		AppSettings.motion_interval = request.getPostParameter("motion_interval").toInt();
		AppSettings.motion_topic = request.getPostParameter("topic_motion");

		debugf("Updating Peripheral settings");

		/* Save peripheral config */
		AppSettings.savePeriph();

		/* Start services again */
		startServices();

		/* Set interval of sensor Publishing if timer was started before */
		if (sensorPublishTimer.isStarted()) {
			sensorPublishTimer.setIntervalMs(AppSettings.timer_delay);
		} else {
			if (AppSettings.adc) {
				sensorPublishTimer.setIntervalMs(AppSettings.timer_delay);
				sensorPublishTimer.start();
			}
		}

		/* Set interval of motion sensing sensor check if timer was started before */
		if (motionCheckTimer.isStarted()) {
			motionCheckTimer.setIntervalMs(AppSettings.motion_interval);
		} else {
			if (AppSettings.motion) {
				motionCheckTimer.initializeMs(AppSettings.motion_interval, motionSensorCheck).start();
			}
		}
	}

	AppSettings.loadPeriph();

	TemplateFileStream *tmpl = new TemplateFileStream("periph.html");
	auto &vars = tmpl->variables();

	vars["timer_delay"] = String(AppSettings.timer_delay);

	/* Relay Settings */
	vars["relay_on"] = AppSettings.relay ? "checked='checked'" : "";
	vars["relay_pin"] = AppSettings.relay_pin;
	vars["relay_invert"] = AppSettings.relay_invert ? "checked='checked'" : "";
	vars["topic_relay_cmd"] = AppSettings.relay_topic_cmd;
	vars["topic_relay_pub"] = AppSettings.relay_topic_pub;
	vars["keyinput_on"] = AppSettings.keyinput ? "checked='checked'" : "";
	vars["keyinput_pin"] = AppSettings.keyinput_pin;
	vars["keyinput_invert"] = AppSettings.keyinput_invert ? "checked='checked'" : "";

	/* ADC Settings */
	vars["adc_on"] = AppSettings.adc ? "checked='checked'" : "";
	vars["topic_adc"] = AppSettings.adc_topic;
	vars["adc_pub_on"] = AppSettings.adc_pub ? "checked='checked'" : "";
	vars["adc_pub_off"] = !AppSettings.adc_pub ? "checked='checked'" : "";

	/* RCSwitch Settings */
	vars["rcswitch_on"] = AppSettings.rcswitch ? "checked='checked'" : "";
	vars["topic_rcswitch"] = AppSettings.rcswitch_topic_prefix;
	vars["rcswitch_pin"] = AppSettings.rcswitch_pin;

	/* MAX7219 Settings */
	vars["max7219_on"] = AppSettings.max7219 ? "checked='checked'" : "";
	vars["max7219_count"] = AppSettings.max7219_count;
	vars["max7219_pin"] = AppSettings.max7219_ss_pin;
	vars["max7219_prefix"] = AppSettings.max7219_topic_prefix;
	vars["max7219_enable"] = AppSettings.max7219_topic_enable;
	vars["max7219_text"] = AppSettings.max7219_topic_text;
	vars["max7219_scroll"] = AppSettings.max7219_topic_scroll;
	vars["max7219_speed"] = AppSettings.max7219_topic_speed;
	vars["max7219_char"] = AppSettings.max7219_topic_charwidth;
	vars["max7219_int"] = AppSettings.max7219_topic_intensity;

	vars["motion_on"] = AppSettings.motion ? "checked='checked'" : "";
	vars["motion_pin"] = AppSettings.motion_pin;
	vars["motion_invert"] = AppSettings.motion_invert ? "checked='checked'" : "";
	vars["topic_motion"] = AppSettings.motion_topic;
	vars["motion_interval"] = AppSettings.motion_interval;

	vars["lastedit"] = lastModified;

	vars["status"] = getStatusString();

	response.sendTemplate(tmpl); // will be automatically deleted
}

/* MQTT Config target for Webserver */
void onMQTTConfig(HttpRequest &request, HttpResponse &response)
{
	//TODO: Message in webinterface, mqtt will enabled after first save
	/* If values are passed from browser we will set here */
	if (request.method == HTTP_POST)
	{
		mqtt.setEnabled(true);
		AppSettings.mqtt_server = request.getPostParameter("server");
		AppSettings.mqtt_port = request.getPostParameter("port").toInt();
		AppSettings.mqtt_userid = request.getPostParameter("userid");
		AppSettings.mqtt_login = request.getPostParameter("login");
		AppSettings.mqtt_password = request.getPostParameter("password");
		AppSettings.mqtt_topic_lwt = request.getPostParameter("topic_lwt");
		AppSettings.mqtt_topic_cmd = request.getPostParameter("topic_cmd");
		AppSettings.mqtt_topic_pub = request.getPostParameter("topic_pub");
		AppSettings.mqtt_enabled = mqtt.isEnabled();
		debugf("Updating MQTT settings: %d", AppSettings.mqtt_server.length());
		AppSettings.saveMQTT();

	}

	TemplateFileStream *tmpl = new TemplateFileStream("mqtt.html");
	auto &vars = tmpl->variables();

	if (AppSettings.mqtt_server.length() <= 0) { vars["server"] = DEFAULT_MQTT_SERVER; } else { vars["server"] = AppSettings.mqtt_server; }
	if (AppSettings.mqtt_port > 1024) { vars["port"] = DEFAULT_MQTT_PORT; } else { vars["port"] = AppSettings.mqtt_port; }
	if (AppSettings.mqtt_login.length() <= 0) { vars["login"] = DEFAULT_MQTT_LOGIN; } else { vars["login"] = AppSettings.mqtt_login; }
	if (AppSettings.mqtt_userid.length() <= 0) { vars["userid"] = DEFAULT_MQTT_USERID; } else { vars["userid"] = AppSettings.mqtt_userid; }
	if (AppSettings.mqtt_password.length() <= 0) { vars["password"] = DEFAULT_MQTT_PASS; } else { vars["password"] = AppSettings.mqtt_password; }
	if (AppSettings.mqtt_topic_lwt.length() <= 0) { vars["topic_lwt"] = DEFAULT_MQTT_LWT; } else { vars["topic_lwt"] = AppSettings.mqtt_topic_lwt; }
	if (AppSettings.mqtt_topic_cmd.length() <= 0) { vars["topic_cmd"] = DEFAULT_MQTT_CMD; } else { vars["topic_cmd"] = AppSettings.mqtt_topic_cmd; }
	if (AppSettings.mqtt_topic_pub.length() <= 0) { vars["topic_pub"] = DEFAULT_MQTT_PUB; } else { vars["topic_pub"] = AppSettings.mqtt_topic_pub; }

	vars["lastedit"] = lastModified;

	vars["status"] = getStatusString();

	response.sendTemplate(tmpl); // will be automatically deleted
}

/* Default target for Webserver */
void onFile(HttpRequest &request, HttpResponse &response)
{
	if (lastModified.length() >0 && request.getHeader("If-Modified-Since").equals(lastModified)) {
		response.code = HTTP_STATUS_NOT_MODIFIED;
		return;
	}

	String file = request.getPath();
	if (file[0] == '/')
		file = file.substring(1);

	if (file[0] == '.')
		response.forbidden();
	else
	{
		if(lastModified.length() > 0) {
			response.setHeader("Last-Modified", lastModified);
		}

		response.setCache(86400, true); // It's important to use cache for better performance.
		response.sendFile(file);
	}
}

/* Debugging configuration values, dumps out the whole JSON config file which is currently active */
/* Syntax: /dumpcfg?type=<type>   where type is the type of the config file (network, mqtt, periph) */
void onDumpConfig(HttpRequest &request, HttpResponse &response)
{
	if (request.method == HTTP_GET) {
		String type = request.getQueryParameter("type");
		if (type.equals("network")) {
			int size = fileGetSize(APP_GLOBAL_SETTINGS_FILE);
			char* cfgString = new char[size + 1];
			fileGetContent(APP_GLOBAL_SETTINGS_FILE, cfgString, size + 1);

			response.sendString(cfgString);
		} else if (type.equals("mqtt")) {
			int size = fileGetSize(APP_MQTT_SETTINGS_FILE);
			char* cfgString = new char[size + 1];
			fileGetContent(APP_MQTT_SETTINGS_FILE, cfgString, size + 1);

			response.sendString(cfgString);
			delete[] cfgString;
		} else if (type.equals("periph")) {
			int size = fileGetSize(APP_PERIPH_SETTINGS_FILE);
			char* cfgString = new char[size + 1];
			fileGetContent(APP_PERIPH_SETTINGS_FILE, cfgString, size + 1);

			response.sendString(cfgString);
			delete[] cfgString;
		}
	} else {
		int size1 = fileGetSize(APP_GLOBAL_SETTINGS_FILE);
		int size2 = fileGetSize(APP_MQTT_SETTINGS_FILE);
		int size3 = fileGetSize(APP_PERIPH_SETTINGS_FILE);
		char* cfgString1 = new char[size1 + 1];
		char* cfgString2 = new char[size2 + 1];
		char* cfgString3 = new char[size3 + 1];
		String delimiter("\r\n\r\n-\r\n\r\n");
		fileGetContent(APP_GLOBAL_SETTINGS_FILE, cfgString1, size1 + 1);
		fileGetContent(APP_MQTT_SETTINGS_FILE, cfgString2, size2 + 1);
		fileGetContent(APP_PERIPH_SETTINGS_FILE, cfgString3, size3 + 1);
		String temp = String(cfgString1) + delimiter + String(cfgString2) + delimiter + String(cfgString3);
		response.sendString(temp);
		delete[] cfgString1;
		delete[] cfgString2;
		delete[] cfgString3;
	}
}

/* Ajax target on Network List receive */
void onAjaxNetworkList(HttpRequest &request, HttpResponse &response)
{
	/* Create JSON stream */
	JsonObjectStream* stream = new JsonObjectStream();
	JsonObject& json = stream->getRoot();

	json["status"] = (bool)true;

	/* Get connection state of wifi station */
	bool connected = WifiStation.isConnected();
	json["connected"] = connected;
	if (connected)
	{
		/* Copy full string to JSON buffer memory */
		json["network"]= WifiStation.getSSID();
	}

	JsonArray& netlist = json.createNestedArray("available");
	for (int i = 0; i < networks.count(); i++)
	{
		if (networks[i].hidden) continue;
		JsonObject &item = netlist.createNestedObject();
		item["id"] = (int)networks[i].getHashId();
		/* Copy full string to JSON buffer memory */
		item["title"] = networks[i].ssid;
		item["signal"] = networks[i].rssi;
		item["encryption"] = networks[i].getAuthorizationMethodName();
	}

	response.setAllowCrossDomainOrigin("*");

	/* Send back JSON created network list to our ajax script */
	response.sendDataStream(stream, MIME_JSON);
}

/* Start connection triggered from ajax service */
void makeConnection()
{
	/* Enable wifistation if not already enabled */
	WifiStation.enable(true);

	/* Set config for wifistation from values */
	WifiStation.config(network, password, true, false);
	/* Run our method when station was connected to AP (or not connected) */
	/* Set callback that should be triggered when we have assigned IP */
	WifiEvents.onStationGotIP(connectOk);

	/* Set callback that should be triggered if we are disconnected or connection attempt failed */
	WifiEvents.onStationDisconnect(connectFail);

	/* Trigger wifistation connection manual */
	WifiStation.connect();

	/* Write values from connection to appsettings and save to config file */
	AppSettings.ssid = network;
	AppSettings.password = password;
	/* Save all config files from values of AppSettings */
	AppSettings.saveNetwork();
	debugf("Making Connection");

	/* Small delay to let him connect */
	delay(3000);

	/* Set network value back to "" for task completion indication */
	network = "";
}

/* Ajax target to connect to wifi network */
void onAjaxConnect(HttpRequest &request, HttpResponse &response)
{
	JsonObjectStream* stream = new JsonObjectStream();
	JsonObject& json = stream->getRoot();

	String curNet = request.getPostParameter("network");
	String curPass = request.getPostParameter("password");

	bool updating = curNet.length() > 0 && (WifiStation.getSSID() != curNet || WifiStation.getPassword() != curPass);
	bool connectingNow = WifiStation.getConnectionStatus() == eSCS_Connecting || network.length() > 0;

	if (updating && connectingNow)
	{
		debugf("wrong action: %s %s, (updating: %d, connectingNow: %d)", network.c_str(), password.c_str(), updating, connectingNow);
		json["status"] = (bool)false;
		json["connected"] = (bool)false;
	}
	else
	{
		json["status"] = (bool)true;
		if (updating)
		{
			network = curNet;
			password = curPass;
-			debugf("CONNECT TO: %s %s", network.c_str(), password.c_str());
			json["connected"] = false;
			connectionTimer.initializeMs(1200, makeConnection).startOnce();
		}
		else
		{
			json["connected"] = WifiStation.isConnected();
			debugf("Network already selected. Current status: %s", WifiStation.getConnectionStatusName());
		}
	}

	if (!updating && !connectingNow && WifiStation.isConnectionFailed())
		json["error"] = WifiStation.getConnectionStatusName();

	response.setAllowCrossDomainOrigin("*");
	response.sendDataStream(stream, MIME_JSON);
}

/* Starts the implemented Webserver and add the used targets */
void startWebServer()
{
	/* Listen on Port */
	//TODO: Webserver port in configuration file?
	server.listen(80);

	/* Add targets to server */
	server.addPath("/", onIndex);
	server.addPath("/reboot", onReboot);
	server.addPath("/config", onConfig);
	server.addPath("/periph", onPeriphConfig);
	server.addPath("/dumpcfg", onDumpConfig);
	server.addPath("/mqttconfig", onMQTTConfig);
	server.addPath("/ajax/get-networks", onAjaxNetworkList);
	server.addPath("/ajax/connect", onAjaxConnect);

	/* Any other targets */
	server.setDefaultHandler(onFile);
}

/* Starts the FTP Server with fixed credentials, just for debugging purposes, had to be disabled in production state */
void startFTP()
{
	/* Check for existing index.html, else create a empty one with error message */
	if (!fileExist("index.html"))
		fileSetContent("index.html", "<h3>No SPIFFS found! Contact supplier!</h3>");

	/* Start FTP server instance */
	ftp.listen(DEFAULT_FTP_PORT);

	/* Add fixed credention ftp user account */
	ftp.addUser(DEFAULT_FTP_USER, DEFAULT_FTP_PASS); // FTP account
}

/* Start mDNS Server using ESP8266 SDK functions */
/*
void startmDNS(IPAddress ip)
{
    struct mdns_info *info = (struct mdns_info *)os_zalloc(sizeof(struct mdns_info));
    info->host_name = (char *) DEFAULT_MDNS_HOSTNAME; // You can replace test with your own host name
    info->ipAddr = ip;
    info->server_name = (char *) DEFAULT_MDNS_SERVERNAME;
    info->server_port = DEFAULT_MDNS_SERVERPORT;
    info->txt_data[0] = (char *) "version = now";
    espconn_mdns_init(info);
}

// Stop mDNS Server
void stopmDNS()
{
	espconn_mdns_close();
}
*/

/* Will be called when system initialization was completed */
void startServers()
{
	startFTP();
	startWebServer();
}

/* Sets Wifi Accesspoint Settings and start it */
void startAP()
{
	/* Put on Setup LED Indicator to indicate running ap */
	statusLed(true);

	/* Start AP for configuration */
	WifiAccessPoint.enable(true);
	WifiAccessPoint.config(NINHOME_AP_NAME, "", AUTH_OPEN);

	//TODO: activate mDNS on startAP()
	/* Start mDNS server on Wifi Accesspoint IP */
	//startmDNS(WifiAccessPoint.getIP());
}

/* Stop Wifi Accesspoint if running */
void stopAP()
{
	/* If Wifi Accesspoint is enabled then disable it */
	if (WifiAccessPoint.isEnabled())
		WifiAccessPoint.enable(false);

	/* Put off Setup LED Indicator */
	statusLed(false);

	//TODO: disable mDNS server on stopAP()
	/* Stop mDNS server */
	//stopmDNS();
}

/* Callback when wifi network scan is finished */
void networkScanCompleted(bool succeeded, BssList list)
{
	/* if the network scan was successful */
	if (succeeded)
	{
		/* populate the networks list with the found networks */
		for (int i = 0; i < list.count(); i++)
			if (!list[i].hidden && list[i].ssid.length() > 0) {
				networks.add(list[i]);
				debugf("Network %d = %s", i, list[i].ssid.c_str());
			}
	}
	/* sort the found networks */
	networks.sort([](const BssInfo& a, const BssInfo& b){ return b.rssi - a.rssi; } );
}

/* Callback when WiFi station was connected to AP and got IP */
void connectOk(IPAddress ip, IPAddress mask, IPAddress gateway)
{
	/* Debug output of IP */
	debugf("I'm CONNECTED");
	Serial.println(ip.toString());

	AppSettings.loadMQTT();
	mqtt.setEnabled(AppSettings.mqtt_enabled);

	/* Start MQTT client and publish/subscribe used extensions */
	if (mqtt.isEnabled()) {
		debugf("Starting MQTT Client");
		startMqttClient();
	} else {
		debugf("MQTT still disabled, update settings!");
	}

	//TODO: Is another load of peripheral settings necessary?
	/* Load peripheral settings */
	AppSettings.loadPeriph();

	/* Start Services for Peripherals */
	startServices();

	/* Start Timer for publishing attached sensor values, interval from settings */
	/* Add every sensor who want to publish something here */
	if (AppSettings.adc) {
		sensorPublishTimer.initializeMs(AppSettings.timer_delay, sensorPublish).start();
	}

	if (AppSettings.motion) {
		motionCheckTimer.initializeMs(AppSettings.motion_interval, motionSensorCheck).start();
	}

	/* Start timer which checks the connection to MQTT */
	checkConnectionTimer.initializeMs(DEFAULT_CONNECT_CHECK_INTERVAL, checkMQTTConnection).start();

	//stopAP(); TODO: When to stop AP? Or manually triggered in webinterface?
}

/* Callback when WiFi station timeout was reached */
void connectFail(String ssid, uint8_t ssidLength, uint8_t *bssid, uint8_t reason)
{
	/* The different reason codes can be found in user_interface.h. in your SDK. */
	debugf("Disconnected from %s. Reason: %d", ssid.c_str(), reason);
	/* If wifi disconnect reason was authentication failed then assume wrong password and start wifi accesspoint again */
	if (reason >= 200) {//|| reason == 2) {
		/* Disconnect wifistation, dont know if necessary */
		WifiStation.disconnect();
		debugf("Falling Back to Setup Mode");
		/* Start Wifi Accesspoint */
		startAP();
	} else {
		/* Try to reconnect */
		debugf("Trying to reconnect to Wifi");
		WifiStation.connect();
	}
}

void statusLed(bool state)
{
	if (AppSettings.status_led_pin > -1) {
		pinMode(AppSettings.status_led_pin, OUTPUT);
		if (AppSettings.status_led_inv)
			state = !state;
		digitalWrite(AppSettings.status_led_pin, state);
	}
}

bool getStatusLed()
{
	if (AppSettings.status_led_pin > -1) {
		if (AppSettings.status_led_inv)
			return !digitalRead(AppSettings.status_led_pin);
		else
			return digitalRead(AppSettings.status_led_pin);
	} else {
		return false;
	}
}

/* Initializes the ninHome Node and start attached peripherals */
void initNode()
{
	/* Load Last Modified Timestamp from the Webcontent */
	if(fileExist(".lastModified")) {
		// The last modification
		lastModified = fileGetContent(".lastModified");
		lastModified.trim();
	}

	/* Enable Wifi Client, even if none is set. Just to scan out existing networks */
	WifiStation.enable(true, false);
	/* Enable Wifi Scanning and process in "networkScanCompleted" callback function */
	WifiStation.startScan(networkScanCompleted);

	/* Check if there is already an existing configuration for wifi network */
	if (AppSettings.existGlobal())
	{
		/* Load other configuration files */
		AppSettings.loadAll();

		statusLed(false);

		/* Blink Setup LED 3 Times to indicate starting */
		for (uint8_t i = 1; i <= 6; i++) {
			statusLed(!getStatusLed());
			delay(200);
		}

		/* If there is any ssid set then assume there is a network set */
		if (AppSettings.ssid.length() > 0) {
			debugf("Settings found. Starting Station Mode");
			/* Stopping Accesspoint mode if it is running */
			stopAP();

			//TODO: nosave option wont work correctly
			/* Set the config values to the corrosponding wifistation settings and disable saving */
			WifiStation.config(AppSettings.ssid, AppSettings.password, true, false);

			/* If dhcp is disabled in settings then enable set fixed ip from settings */
			if (!AppSettings.dhcp && !AppSettings.ip.isNull())
				WifiStation.setIP(AppSettings.ip, AppSettings.netmask, AppSettings.gateway);

			/* register callbacks for GotIP and Disconnect wifi events */
			WifiEvents.onStationGotIP(connectOk);
			/* set callback that should be triggered if we are disconnected or connection attempt failed */
			WifiEvents.onStationDisconnect(connectFail);
		} else {
			/* No valid settings found, so start access point mode */
			debugf("Settings found but SSID is empty! Starting Setup Mode");
			startAP();
		}
	} else {
		/* No settings file found, so start access point mode */
		debugf("No Settings found. Starting Setup Mode");
		startAP();
	}

	/* If a motion sensor is attached, enable the input gpio */
	if (AppSettings.motion)
		pinMode(AppSettings.motion_pin, INPUT_PULLUP);

	/* If a relay attached and enabled in settings we init it here */
	if (AppSettings.relay)
		relay.init(AppSettings.relay_pin, AppSettings.relay_status_pin, AppSettings.relay_invert, false);

	if (AppSettings.max7219) {
		led.init(AppSettings.max7219_count, AppSettings.max7219_ss_pin);
		led.setIntensity(DEFAULT_MAX7219_INTENSITY); // range is 0-15
		led.setText(DEFAULT_MAX7219_TEXT);
		led.setDeviceOrientation(AppSettings.max7219_orientation);
	}

	/* Register onReady callback to run services on system ready */
	System.onReady(startServers);
}

void motionSensorCheck()
{
	if (digitalRead(AppSettings.motion_pin) == AppSettings.motion_invert) {
		if (motionState != true) {
			motionState = true;
			if (mqtt.getConnectionState() != eTCS_Connected) //TODO remove autoconnect cause of connCheck timer?
						startMqttClient(); // Auto reconnect
			mqtt.publishWithQoS(AppSettings.motion_topic, "1", 1, true);
			debugf("Motion ON (%s)", AppSettings.motion_topic.c_str());
		}
		//digitalWrite(14, 1);
	} else {
		if (motionState != false) {
			motionState = false;
			if (mqtt.getConnectionState() != eTCS_Connected)
						startMqttClient(); // Auto reconnect
			mqtt.publishWithQoS(AppSettings.motion_topic, "0", 1, true);
			debugf("Motion OFF (%s)", AppSettings.motion_topic.c_str());
		}
		//digitalWrite(14, 0);
	}
}

void init()
{
	/* Mount file system, in order to work with files */
	spiffs_mount();

	/* Start Serial Debug Terminal */
	Serial.begin(115200); // 115200 by default

	//TODO: if debug disabled it get into bootloop, inspect stacktrace
	/* Enable debug output to serial */
	Serial.systemDebugOutput(true);

	/*pinMode(12, INPUT_PULLUP);
	pinMode(14, OUTPUT);

	testTimer.initializeMs(10, blupp).start();
	 */

	/* Start initialization of ninHOME node */
	initNode();
}
