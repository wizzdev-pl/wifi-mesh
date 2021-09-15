#ifndef MQTT_APP_H_
#define MQTT_APP_H_

void MQTT_AppStart(void);
void MQTT_AppPublish(const char* pTopic, const char* pPublishString);

#define MQTT_BUTTON_TOPIC "/topic/03c8b0f712023b6d/ip_mesh/key_pressed" //topic randomized to avoid conflict with Espressif example


#endif // MQTT_APP_H_
