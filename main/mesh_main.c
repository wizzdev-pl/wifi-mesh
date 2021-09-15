#include "mesh_netif.h"
#include "mqtt_app.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include <string.h> // for strlen,memcpy

#ifdef CONFIG_IDF_TARGET_ESP32C3
#define EXAMPLE_BUTTON_GPIO     9 // esp32 c3 boot button
#else
#define EXAMPLE_BUTTON_GPIO     0 // esp32 boot button
#endif


#define MESH_ID_SIZE 6

// commands for internal mesh communication:
// <CMD> <PAYLOAD>, where CMD is one character, payload is variable dep. on command
#define CMD_KEYPRESSED 0x55
// CMD_KEYPRESSED: payload is always 6 bytes identifying address of node sending keypress event
#define CMD_KEYPRESSED_PAYLOAD_SIZE MESH_ID_SIZE
#define CMD_ROUTE_TABLE 0x56
// CMD_ROUTE_TABLE: payload is a multiple of 6 listing addresses in a routing table
#define CMD_ROUTE_TABLE_SIZE_PER_ENTRY 6

#define COMMAND_SIZE 1

#define MACSTR_FMT MACSTR
#define MACSTR_LEN ((6 * 3 -1) + 1) // MAC address string size in hex with seperators + terminator

static const char* MESH_TAG = "mesh_main";
static const uint8_t MESH_ID[MESH_ID_SIZE] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x76 };

typedef struct
{
    mesh_addr_t MeshParentAddr;
    int MeshLayer;
    esp_ip4_addr_t currentIp;
    mesh_addr_t RouteTable[CONFIG_MESH_ROUTE_TABLE_SIZE];
    int RouteTableSize;
    SemaphoreHandle_t routeTableLock;
    uint8_t MeshTxPayload[CONFIG_MESH_ROUTE_TABLE_SIZE * CMD_ROUTE_TABLE_SIZE_PER_ENTRY + COMMAND_SIZE];
} meshMainStruct_t;

static meshMainStruct_t meshMainStruct = { .MeshLayer = -1 };

#define BUTTON_POLL_PERIOD_ms 50

static void initialiseButton(void)
{
    gpio_config_t io_conf = { .pin_bit_mask = BIT64(EXAMPLE_BUTTON_GPIO), .mode = GPIO_MODE_INPUT, .pull_up_en = 1 };
    gpio_config(&io_conf);
}

void static MeshReceiveCb(mesh_addr_t* from, mesh_data_t* data)
{
    if (data->data[0] == CMD_ROUTE_TABLE)
    {
        int size = data->size - COMMAND_SIZE;// ignore command byte in payload size
        if ((meshMainStruct.routeTableLock == NULL) || ((size % CMD_ROUTE_TABLE_SIZE_PER_ENTRY) != 0))
        {
            ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unexpected size");
            return;
        }
        xSemaphoreTake(meshMainStruct.routeTableLock, portMAX_DELAY);
        meshMainStruct.RouteTableSize = size / CMD_ROUTE_TABLE_SIZE_PER_ENTRY;
        for (int i = 0; i < meshMainStruct.RouteTableSize; ++i)
        {
            ESP_LOGI(MESH_TAG, "Received Routing table [%d] " MACSTR, i,
                    MAC2STR(data->data + CMD_ROUTE_TABLE_SIZE_PER_ENTRY*i + 1));
        }
        memcpy(&meshMainStruct.RouteTable, data->data + COMMAND_SIZE, size);
        xSemaphoreGive(meshMainStruct.routeTableLock);
    }
    else if (data->data[0] == CMD_KEYPRESSED)
    {
        if (data->size != (COMMAND_SIZE + CMD_KEYPRESSED_PAYLOAD_SIZE))
        {
            ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unexpected size");
            return;
        }
        ESP_LOGW(MESH_TAG, "Keypressed detected on node: " MACSTR_FMT, MAC2STR(data->data + COMMAND_SIZE));
    }
    else
    {
        ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unknown command");
    }
}

static void CheckButton(void* args)
{
    static bool oldLevel = true;
    bool runCheckButton = true;
    initialiseButton();
    while (runCheckButton)
    {
        bool newLevel = gpio_get_level(EXAMPLE_BUTTON_GPIO);
        if (!newLevel && oldLevel)
        {
            if (meshMainStruct.RouteTableSize && !esp_mesh_is_root())
            {
                ESP_LOGW(MESH_TAG, "Key pressed!");
                mesh_data_t data;
                uint8_t* pMyMAC = meshNetifGetStationMAC();
                uint8_t txData[COMMAND_SIZE + CMD_KEYPRESSED_PAYLOAD_SIZE] = { CMD_KEYPRESSED, };
                char MAC_String[MACSTR_LEN];
                memcpy(txData + COMMAND_SIZE, pMyMAC, CMD_KEYPRESSED_PAYLOAD_SIZE);
                data.size = sizeof(txData);
                data.proto = MESH_PROTO_BIN;
                data.tos = MESH_TOS_P2P;
                data.data = txData;

                snprintf(MAC_String, sizeof(MAC_String), MACSTR_FMT, MAC2STR(pMyMAC));
                MQTT_AppPublish(MQTT_BUTTON_TOPIC, MAC_String);

                xSemaphoreTake(meshMainStruct.routeTableLock, portMAX_DELAY);
                for (int i = 0; i < meshMainStruct.RouteTableSize; i++)
                {
                    if (MAC_ADDR_EQUAL(meshMainStruct.RouteTable[i].addr, pMyMAC))
                    {
                        continue;
                    }
                    esp_err_t err = esp_mesh_send(&meshMainStruct.RouteTable[i], &data, MESH_DATA_P2P, NULL, 0);
                    ESP_LOGI(MESH_TAG, "Sending to [%d] " MACSTR_FMT ": sent with err code: %d", i,
                            MAC2STR(meshMainStruct.RouteTable[i].addr), err);
                }
                xSemaphoreGive(meshMainStruct.routeTableLock);
            }
        }
        oldLevel = newLevel;
        vTaskDelay(BUTTON_POLL_PERIOD_ms / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void EspMeshMQTT_Task(void* arg)
{
    char* pPrintBuffer;
    mesh_data_t data;
    esp_err_t err;
    MQTT_AppStart();
    while (1)
    {
        asprintf(&pPrintBuffer, "layer:%d IP:" IPSTR, esp_mesh_get_layer(), IP2STR(&meshMainStruct.currentIp));
        ESP_LOGI(MESH_TAG, "Tried to publish %s", pPrintBuffer);
        MQTT_AppPublish("/topic/ip_mesh", pPrintBuffer);
        free(pPrintBuffer);
        if (esp_mesh_is_root())
        {
            esp_mesh_get_routing_table((mesh_addr_t*) &meshMainStruct.RouteTable, CONFIG_MESH_ROUTE_TABLE_SIZE * 6,
                    &meshMainStruct.RouteTableSize);
            data.size = meshMainStruct.RouteTableSize * 6 + 1;
            data.proto = MESH_PROTO_BIN;
            data.tos = MESH_TOS_P2P;
            meshMainStruct.MeshTxPayload[0] = CMD_ROUTE_TABLE;
            memcpy(meshMainStruct.MeshTxPayload + 1, meshMainStruct.RouteTable, meshMainStruct.RouteTableSize * 6);
            data.data = meshMainStruct.MeshTxPayload;
            for (int i = 0; i < meshMainStruct.RouteTableSize; i++)
            {
                err = esp_mesh_send(&meshMainStruct.RouteTable[i], &data, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(MESH_TAG, "Sending routing table to [%d] " MACSTR_FMT ": sent with err code: %d", i,
                        MAC2STR(meshMainStruct.RouteTable[i].addr), err);
            }
        }
        vTaskDelay(2 * 1000 / portTICK_RATE_MS);
    }
    vTaskDelete(NULL);
}

esp_err_t EspMeshCommMQTT_TaskStart(void)
{
    static bool isCommMQTT_TaskStarted = false;

    meshMainStruct.routeTableLock = xSemaphoreCreateMutex();

    if (!isCommMQTT_TaskStarted)
    {
        xTaskCreate(EspMeshMQTT_Task, "mqtt task", 3072, NULL, 5, NULL);
        xTaskCreate(CheckButton, "check button task", 3072, NULL, 5, NULL);
        isCommMQTT_TaskStarted = true;
    }
    return ESP_OK;
}

void MeshEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* pEventData)
{
    mesh_addr_t id = { 0, };
    static uint8_t lastLayer = 0;

    switch (event_id)
    {
        case MESH_EVENT_STARTED:
        {
            esp_mesh_get_id(&id);
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:"MACSTR_FMT"", MAC2STR(id.addr));
            meshMainStruct.MeshLayer = esp_mesh_get_layer();
            break;
        }
        case MESH_EVENT_STOPPED:
        {
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
            meshMainStruct.MeshLayer = esp_mesh_get_layer();
            break;
        }
        case MESH_EVENT_CHILD_CONNECTED:
        {
            mesh_event_child_connected_t* pChildConnected = (mesh_event_child_connected_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR_FMT"", pChildConnected->aid,
                    MAC2STR(pChildConnected->mac));
            break;
        }
        case MESH_EVENT_CHILD_DISCONNECTED:
        {
            mesh_event_child_disconnected_t* pChildDisconnected = (mesh_event_child_disconnected_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR_FMT"", pChildDisconnected->aid,
                    MAC2STR(pChildDisconnected->mac));
            break;
        }
        case MESH_EVENT_ROUTING_TABLE_ADD:
        {
            mesh_event_routing_table_change_t* pRoutingTable = (mesh_event_routing_table_change_t*) pEventData;
            ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d", pRoutingTable->rt_size_change,
                    pRoutingTable->rt_size_new);
            break;
        }
        case MESH_EVENT_ROUTING_TABLE_REMOVE:
        {
            mesh_event_routing_table_change_t* pRoutingTable = (mesh_event_routing_table_change_t*) pEventData;
            ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d", pRoutingTable->rt_size_change,
                    pRoutingTable->rt_size_new);
            break;
        }
        case MESH_EVENT_NO_PARENT_FOUND:
        {
            mesh_event_no_parent_found_t* pNoParent = (mesh_event_no_parent_found_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d", pNoParent->scan_times);
            break;
        }
/* TODO handler for the failure */
        case MESH_EVENT_PARENT_CONNECTED:
        {
            mesh_event_connected_t* pConnected = (mesh_event_connected_t*) pEventData;
            esp_mesh_get_id(&id);
            meshMainStruct.MeshLayer = pConnected->self_layer;
            memcpy(&meshMainStruct.MeshParentAddr.addr, pConnected->connected.bssid, 6);
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR_FMT"%s, ID:"MACSTR_FMT"",
                    lastLayer, meshMainStruct.MeshLayer, MAC2STR(meshMainStruct.MeshParentAddr.addr),
                    esp_mesh_is_root() ? "<ROOT>" : (meshMainStruct.MeshLayer == 2) ? "<layer2>" : "",
                    MAC2STR(id.addr));
            lastLayer = meshMainStruct.MeshLayer;
            meshNetifsStart(esp_mesh_is_root());
            break;
        }
        case MESH_EVENT_PARENT_DISCONNECTED:
        {
            mesh_event_disconnected_t* pDisconnected = (mesh_event_disconnected_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d", pDisconnected->reason);
            meshMainStruct.MeshLayer = esp_mesh_get_layer();
            meshNetifsStop();
            break;
        }
        case MESH_EVENT_LAYER_CHANGE:
        {
            mesh_event_layer_change_t* pLayerChange = (mesh_event_layer_change_t*) pEventData;
            meshMainStruct.MeshLayer = pLayerChange->new_layer;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s", lastLayer, meshMainStruct.MeshLayer,
                    esp_mesh_is_root() ? "<ROOT>" : (meshMainStruct.MeshLayer == 2) ? "<layer2>" : "");
            lastLayer = meshMainStruct.MeshLayer;
            break;
        }
        case MESH_EVENT_ROOT_ADDRESS:
        {
            mesh_event_root_address_t* pRootAddress = (mesh_event_root_address_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR_FMT"", MAC2STR(pRootAddress->addr));
            break;
        }
        case MESH_EVENT_VOTE_STARTED:
        {
            mesh_event_vote_started_t* pVoteStarted = (mesh_event_vote_started_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR_FMT"",
                    pVoteStarted->attempts, pVoteStarted->reason, MAC2STR(pVoteStarted->rc_addr.addr));
            break;
        }
        case MESH_EVENT_VOTE_STOPPED:
        {
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
            break;
        }
        case MESH_EVENT_ROOT_SWITCH_REQ:
        {
            mesh_event_root_switch_req_t* pSwitchRequest = (mesh_event_root_switch_req_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR_FMT"", pSwitchRequest->reason,
                    MAC2STR( pSwitchRequest->rc_addr.addr));
            break;
        }
        case MESH_EVENT_ROOT_SWITCH_ACK:
        {
/* new root */
            meshMainStruct.MeshLayer = esp_mesh_get_layer();
            esp_mesh_get_parent_bssid(&meshMainStruct.MeshParentAddr);
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR_FMT"", meshMainStruct.MeshLayer,
                    MAC2STR(meshMainStruct.MeshParentAddr.addr));
            break;
        }
        case MESH_EVENT_TODS_STATE:
        {
            // toDS state, devices shall check this state firstly before trying to send packets to external IP network.
            // This state indicates right now whether the root is capable of sending packets out.
            mesh_event_toDS_state_t* pToDsState = (mesh_event_toDS_state_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *pToDsState);
            break;
        }
        case MESH_EVENT_ROOT_FIXED:
        {
            mesh_event_root_fixed_t* pRootFixed = (mesh_event_root_fixed_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s", pRootFixed->is_fixed ? "fixed" : "not fixed");
            break;
        }
        case MESH_EVENT_ROOT_ASKED_YIELD:
        {
            mesh_event_root_conflict_t* pRootConflict = (mesh_event_root_conflict_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR_FMT", rssi:%d, capacity:%d",
                    MAC2STR(pRootConflict->addr), pRootConflict->rssi, pRootConflict->capacity);
            break;
        }
        case MESH_EVENT_CHANNEL_SWITCH:
        {
            mesh_event_channel_switch_t* pChannelSwitch = (mesh_event_channel_switch_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", pChannelSwitch->channel);
            break;
        }
        case MESH_EVENT_SCAN_DONE:
        {
            mesh_event_scan_done_t* pScanDone = (mesh_event_scan_done_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d", pScanDone->number);
            break;
        }
        case MESH_EVENT_NETWORK_STATE:
        {
            mesh_event_network_state_t* pNetworkState = (mesh_event_network_state_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d", pNetworkState->is_rootless);
            break;
        }
        case MESH_EVENT_STOP_RECONNECTION:
        {
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
            break;
        }
        case MESH_EVENT_FIND_NETWORK:
        {
            mesh_event_find_network_t* pFindNetwork = (mesh_event_find_network_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR_FMT"",
                    pFindNetwork->channel, MAC2STR(pFindNetwork->router_bssid));
            break;
        }
        case MESH_EVENT_ROUTER_SWITCH:
        {
            mesh_event_router_switch_t* pRouterSwitch = (mesh_event_router_switch_t*) pEventData;
            ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR_FMT"", pRouterSwitch->ssid,
                    pRouterSwitch->channel, MAC2STR(pRouterSwitch->bssid));
            break;
        }
        default:
        {
            ESP_LOGI(MESH_TAG, "unknown id:%d", event_id);
            break;
        }
    }
}

void ipEventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* pEventData)
{
    ip_event_got_ip_t* pEvent = (ip_event_got_ip_t*) pEventData;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&pEvent->ip_info.ip));
    meshMainStruct.currentIp.addr = pEvent->ip_info.ip.addr;
    esp_netif_t* pNetif = pEvent->esp_netif;
    esp_netif_dns_info_t dns;
    ESP_ERROR_CHECK(esp_netif_get_dns_info(pNetif, ESP_NETIF_DNS_MAIN, &dns));
    meshNetifStartRootAP(esp_mesh_is_root(), dns.ip.u_addr.ip4.addr);
    EspMeshCommMQTT_TaskStart();
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
/*  tcpip initialization */
    ESP_ERROR_CHECK(esp_netif_init());
/*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
/*  crete network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(meshNetifsInit(MeshReceiveCb));

/*  wifi initialization */
    wifi_init_config_t wifiConfig = WIFI_INIT_CONFIG_DEFAULT()
    ;
    ESP_ERROR_CHECK(esp_wifi_init(&wifiConfig));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ipEventHandler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
/*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &MeshEventHandler, NULL));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
    mesh_cfg_t meshConfig = MESH_INIT_CONFIG_DEFAULT();
/* mesh ID */
    memcpy((uint8_t*) &meshConfig.mesh_id, MESH_ID, 6);
/* router */
    meshConfig.channel = CONFIG_MESH_CHANNEL;
    meshConfig.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t*) &meshConfig.router.ssid, CONFIG_MESH_ROUTER_SSID, meshConfig.router.ssid_len);
    memcpy((uint8_t*) &meshConfig.router.password, CONFIG_MESH_ROUTER_PASSWD, strlen(CONFIG_MESH_ROUTER_PASSWD));
/* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    meshConfig.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    memcpy((uint8_t*) &meshConfig.mesh_ap.password, CONFIG_MESH_AP_PASSWD, strlen(CONFIG_MESH_AP_PASSWD));
    ESP_ERROR_CHECK(esp_mesh_set_config(&meshConfig));
/* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%d, %s\n", esp_get_free_heap_size(),
            esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed");
}
