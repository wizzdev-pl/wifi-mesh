#include "mqtt_app.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include <stddef.h> //for NULL

static const char* TAG = "mesh_mqtt";
static esp_mqtt_client_handle_t MQTT_ClientHandle = NULL;

static esp_err_t MQTT_EventProcess(esp_mqtt_event_handle_t event)
{
    switch (event->event_id)
    {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            if (esp_mqtt_client_subscribe(MQTT_ClientHandle, MQTT_BUTTON_TOPIC, 0) < 0)
            {
                // Disconnect to retry the subscribe after auto-reconnect timeout
                esp_mqtt_client_disconnect(MQTT_ClientHandle);
            }
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            ESP_LOGI(TAG, "TOPIC=%.*s", event->topic_len, event->topic);
            ESP_LOGI(TAG, "DATA=%.*s", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void MQTT_EventCb(void* handlerArgs, esp_event_base_t base, int32_t eventId, void* pEventData)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, eventId);
    MQTT_EventProcess(pEventData);
}

void MQTT_AppPublish(const char* pTopic, const char* pPublishString)
{
    if (MQTT_ClientHandle)
    {
        int msg_id = esp_mqtt_client_publish(MQTT_ClientHandle, pTopic, pPublishString, 0, 1, 0);
        ESP_LOGI(TAG, "sent publish returned msg_id=%d", msg_id);
    }
}

void MQTT_AppStart(void)
{
    #if 1
        esp_mqtt_client_config_t MQTT_config = {
             .uri = "mqtt://mqtt.eclipseprojects.io",
        };
    #else
        esp_mqtt_client_config_t MQTT_config = {
            .uri = "mqtt://test.mosquitto.org",
            .port = 1883
        };
    #endif

    MQTT_ClientHandle = esp_mqtt_client_init(&MQTT_config);

    esp_mqtt_client_register_event(MQTT_ClientHandle, ESP_EVENT_ANY_ID, MQTT_EventCb, MQTT_ClientHandle);
    esp_mqtt_client_start(MQTT_ClientHandle);
}
