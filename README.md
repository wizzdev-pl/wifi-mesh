# WizzDev MESH DEMO
Demo of mesh network of ESP32/ESP32C3 devkits. Based on espressifs example: ip_internal_network.\
The firmware picks root node and creates the routing between all nodes.\
Nodes send data to MQTT broker and to other nodes.\
The root doesn't send button mqtt messages, the other nodes do, mesh driver abstracts the internet connection so all devices use same functions for MQTT.\
Devices are subscribed to the topic and detect messages from mosquitto_pub (you can send them from your PC). Devices devices post their ip addresses.

# Setup

- get at least 2 esp32 devices
- set up a router or hotspot
- set up device type with idf.py set-target esp32 or idf.py set-target esp32c3, switching target clears the configuration
- idf.py menuconfig
- go to Example Configuration
- optional: set up wifi router channel in menuconfig, or leave at 0 (you can check wifi channel with a wifi analyzer such as WiFi Analyser for win 10)*
- set up wifi router security type. Note that "Personal" is the same as "PSK"
- set up wifi router password
- set up mesh password (len >=8)
- flash all esp32 devices. It is best to turn them on at the same time.

*Setting channel to 0 can make scanning the network slower. If the channel of the AP is fixed it is recommended to configure this channel for all nodes.\
After the network has been established the AP channel cannot change unless you set set the allow_channel_switch field in mesh_cfg_t to true.\
Channel switches of the AP will result in the mesh network being offline while all the nodes are switching channels.\
So it is recommended to fix the channel in the router.

# MQTT

Topic is changed from the example of espressif with a hardcoded random 64-bit hex number to prevent conflicts with original demo.\
This can be improved by randomizing it per network too, but that is currently not implemented

if using mosquitto default broker:
```
mosquitto_sub -h mqtt.eclipseprojects.io -t /topic/03c8b0f712023b6d/ip_mesh/# -v
mosquitto_pub -h mqtt.eclipseprojects.io -t /topic/03c8b0f712023b6d/ip_mesh/key_pressed -m "<esp32 mac address>"
```

if using mosquitto test broker:
modify mqtt_app.c mqtt_app_start()\
WARNING: as of 2021-08-26 mosquitto test server doesn't work anymore and gives connection errors
```
mosquitto_sub -h test.mosquitto.org -p 1883 -t /topic/03c8b0f712023b6d/ip_mesh/# -v
mosquitto_pub -h test.mosquitto.org -p 1883 -t /topic/03c8b0f712023b6d/ip_mesh/key_pressed -m "<esp32 mac address>"
```

response of mosquitto_sub:
`/topic/03c8b0f712023b6d/ip_mesh/key_pressed <esp32 mac address>`

# Links
- https://docs.espressif.com/projects/esp-idf/en/v4.1/api-guides/mesh.html
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/esp-wifi-mesh.html#channel-and-router-switching-configuration
- https://docs.espressif.com/projects/esp-idf/en/latest/esp32c3/api-guides/esp-wifi-mesh.html#asynchronous-power-on-reset
- https://github.com/espressif/esp-idf/tree/1d7068e/examples/mesh/ip_internal_network
- https://mosquitto.org/download/
- "MQTIZER" android app for MQTT testing, has chat view (can sometimes receive double messages): https://play.google.com/store/apps/details?id=com.sanyamarya.mqtizermqtt_client&hl=en&gl=US
- "MQTT client" android app for MQTT testing (history of messages doesn't automatically refresh): https://play.google.com/store/apps/details?id=in.dc297.mqttclpro&hl=en&gl=US

# Notes
- the espressif term "leaf node" can be confusing as they are not always leafs in the network, but they are simply nodes in the last permissible layer
- OTA in mesh network?
- https://github.com/espressif/esp-mdf
