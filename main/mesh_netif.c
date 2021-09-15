#include "mesh_netif.h"

#include "esp_log.h"
#include "esp_wifi_netif.h"
#include "lwip/lwip_napt.h"

#include <string.h>  // for memcpy,memcmp

#define RX_SIZE      (1560)



typedef struct{
    esp_netif_driver_base_t base;
    uint8_t sta_mac_addr[MAC_ADDR_LEN];
}meshNetifDriver;

static const char* TAG = "mesh_netif";
const esp_netif_ip_info_t g_mesh_netif_subnet_ip = {// mesh subnet IP info
        .ip = { .addr = ESP_IP4TOADDR(10, 0, 0, 1) }, .gw = { .addr = ESP_IP4TOADDR(10, 0, 0, 1) }, .netmask = { .addr =
                ESP_IP4TOADDR(255, 255, 0, 0) }, };

static esp_netif_t* pNetifSta = NULL;
static esp_netif_t* pNetifAP = NULL;
static bool receiveTaskIsRunning = false;
static mesh_addr_t routeTable[CONFIG_MESH_ROUTE_TABLE_SIZE] = { 0 };
static mesh_raw_recv_cb_t* pMeshRawReceiveCb = NULL;

//  setup DHCP server's DNS OFFER
static esp_err_t setDhcpsDNS(esp_netif_t* pNetif, uint32_t addr)
{
    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = addr;
    dns.ip.type = IPADDR_TYPE_V4;
    dhcps_offer_t DhcpsDnsValue = OFFER_DNS;
    ESP_ERROR_CHECK(
            esp_netif_dhcps_option(pNetif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &DhcpsDnsValue,
                    sizeof(DhcpsDnsValue)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(pNetif, ESP_NETIF_DNS_MAIN, &dns));
    return ESP_OK;
}

static void receiveTask(void* arg)
{
    esp_err_t err;
    mesh_addr_t from;
    int flag = 0;
    mesh_data_t data;
    static uint8_t rx_buf[RX_SIZE] = { 0, };

    ESP_LOGD(TAG, "Receiving task started");
    while (receiveTaskIsRunning)
    {
        data.data = rx_buf;
        data.size = RX_SIZE;
        err = esp_mesh_recv(&from, &data, portMAX_DELAY, &flag, NULL, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Received with err code %d %s", err, esp_err_to_name(err));
            continue;
        }
        if (data.proto == MESH_PROTO_BIN && pMeshRawReceiveCb)
        {
            pMeshRawReceiveCb(&from, &data);
        }
        if (esp_mesh_is_root())
        {
            if (data.proto == MESH_PROTO_AP)
            {
                ESP_LOGD(TAG, "Root received: from: " MACSTR " to " MACSTR " size: %d", MAC2STR((uint8_t*)data.data),
                        MAC2STR((uint8_t*)(data.data+6)), data.size);
                if (pNetifAP)
                {
                    // actual receive to TCP/IP stack
                    esp_netif_receive(pNetifAP, data.data, data.size, NULL);
                }
            }
            else if (data.proto == MESH_PROTO_STA)
            {
                ESP_LOGE(TAG, "Root station Should never receive data from mesh!");
            }
        }
        else
        {
            if (data.proto == MESH_PROTO_AP)
            {
                ESP_LOGD(TAG, "Node AP should never receive data from mesh");
            }
            else if (data.proto == MESH_PROTO_STA)
            {
                ESP_LOGD(TAG, "Node received: from: " MACSTR " to " MACSTR " size: %d", MAC2STR((uint8_t*)data.data),
                        MAC2STR((uint8_t*)(data.data+6)), data.size);
                if (pNetifSta)
                {
// actual receive to TCP/IP stack
                    esp_netif_receive(pNetifSta, data.data, data.size, NULL);
                }
            }
        }
    }
    vTaskDelete(NULL);
}

// Free RX buffer (not used as the buffer is static)
static void MeshFree(void* pDriver, void* pBuffer)
{
    free(pBuffer);
}

// Transmit function variants
static esp_err_t meshNetifTransmitFromRootAP(void* pDriver, void* pBuffer, size_t len)
{
    // Use only to transmit data from root AP to node's AP
    static const uint8_t ethBroadcast[MAC_ADDR_LEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    int routeTableSize = 0;
    meshNetifDriver* meshDriver = pDriver;
    mesh_addr_t destAddr;
    mesh_data_t data;

    ESP_LOGD(TAG, "Sending to node: " MACSTR ", size: %d", MAC2STR((uint8_t*)pBuffer), len);
    memcpy(destAddr.addr, pBuffer, MAC_ADDR_LEN);
    data.data = pBuffer;
    data.size = len;
    data.proto = MESH_PROTO_STA;// sending from root AP -> Node's STA
    data.tos = MESH_TOS_P2P;
    if (MAC_ADDR_EQUAL(destAddr.addr, ethBroadcast))
    {
        ESP_LOGD(TAG, "Broadcasting!");
        esp_mesh_get_routing_table( (mesh_addr_t*) &routeTable,
                                    CONFIG_MESH_ROUTE_TABLE_SIZE * 6,
                                    &routeTableSize);
        for (int i = 0; i < routeTableSize; i++)
        {
            if (MAC_ADDR_EQUAL(routeTable[i].addr, meshDriver->sta_mac_addr))
            {
                ESP_LOGD(TAG, "That was me, skipping!");
                continue;
            }
            ESP_LOGD(TAG, "Broadcast: Sending to [%d] " MACSTR, i, MAC2STR(routeTable[i].addr));
            esp_err_t err = esp_mesh_send(&routeTable[i], &data, MESH_DATA_P2P, NULL, 0);
            if (ESP_OK != err)
            {
                ESP_LOGE(TAG, "Send with err code %d %s", err, esp_err_to_name(err));
            }
        }
    }
    else
    {
        // Standard P2P
        esp_err_t err = esp_mesh_send(&destAddr, &data, MESH_DATA_P2P, NULL, 0);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Send with err code %d %s", err, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t meshNetifTransmitFromRootAP_Wrap(void* pDriver, void* pBuffer, size_t len, void* pNetstackBuffer)
{
    return meshNetifTransmitFromRootAP(pDriver, pBuffer, len);
}

static esp_err_t meshNetifTransmitFromNodeSta(void* pDriver, void* pBuffer, size_t len)
{
    mesh_data_t data;
    ESP_LOGD(TAG, "Sending to root, dest addr: " MACSTR ", size: %d", MAC2STR((uint8_t*)pBuffer), len);
    data.data = pBuffer;
    data.size = len;
    data.proto = MESH_PROTO_AP;// Node's station transmits data to root's AP
    data.tos = MESH_TOS_P2P;
    esp_err_t err = esp_mesh_send(NULL, &data, MESH_DATA_TODS, NULL, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Send with err code %d %s", err, esp_err_to_name(err));
    }
    return err;
}

static esp_err_t meshNetifTransmitFromNodeStaWrap(void* pDriver, void* pBuffer, size_t len, void* pNetifBuffer)
{
    return meshNetifTransmitFromNodeSta(pDriver, pBuffer, len);
}

// Construct and Destruct functions
//
static esp_err_t MeshDriverStartRootAP(esp_netif_t* pEspNetif, void* args)
{
    meshNetifDriver* driver = args;
    driver->base.netif = pEspNetif;
    esp_netif_driver_ifconfig_t driverIfconfig = { .handle = driver, .transmit = meshNetifTransmitFromRootAP,
            .transmit_wrap = meshNetifTransmitFromRootAP_Wrap, .driver_free_rx_buffer = MeshFree };

    return esp_netif_set_driver_config(pEspNetif, &driverIfconfig);
}

static esp_err_t MeshDriverStartNodeSta(esp_netif_t* pEspNetif, void* args)
{
    meshNetifDriver* driver = args;
    driver->base.netif = pEspNetif;
    esp_netif_driver_ifconfig_t driver_ifconfig = { .handle = driver, .transmit = meshNetifTransmitFromNodeSta,
            .transmit_wrap = meshNetifTransmitFromNodeStaWrap, .driver_free_rx_buffer = MeshFree };

    return esp_netif_set_driver_config(pEspNetif, &driver_ifconfig);
}

void MeshDeleteIfDriver(meshNetifDriver* driver)
{
// Stop the task once both drivers are removed
//    receive_task_is_running = true;
    free(driver);
}

meshNetifDriver* MeshCreateIfDriver(bool is_ap, bool is_root)
{
    meshNetifDriver* driver = calloc(1, sizeof(meshNetifDriver));
    if (driver == NULL)
    {
        ESP_LOGE(TAG, "No memory to create a wifi interface handle");
        return NULL;
    }
    if (is_ap && is_root)
    {
        driver->base.post_attach = MeshDriverStartRootAP;
    }
    else if (!is_ap && !is_root)
    {
        driver->base.post_attach = MeshDriverStartNodeSta;
    }
    else
    {
        return NULL;
    }

    if (!receiveTaskIsRunning)
    {
        receiveTaskIsRunning = true;
        xTaskCreate(receiveTask, "netif rx task", 3072, NULL, 5, NULL);
    }

// save station mac address to exclude it from routing-table on broadcast
    esp_wifi_get_mac(WIFI_IF_STA, driver->sta_mac_addr);

    return driver;
}

esp_err_t meshNetifsDestroy(void)
{
    receiveTaskIsRunning = false;
    return ESP_OK;
}

static void meshNetifInitStation(void)
{
// By default create a station that would connect to AP (expecting root to connect to external network)
    esp_netif_config_t configSta = ESP_NETIF_DEFAULT_WIFI_STA();
    pNetifSta = esp_netif_new(&configSta);
    assert(pNetifSta);
    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(pNetifSta));
    ESP_ERROR_CHECK(esp_wifi_set_default_wifi_sta_handlers());
}

// Init by default for both potential root and node
esp_err_t meshNetifsInit(mesh_raw_recv_cb_t* pCb)
{
    meshNetifInitStation();
    pMeshRawReceiveCb = pCb;
    return ESP_OK;

}

/**
 * @brief Starts AP esp-netif link over mesh (root's AP on mesh)
 */
static esp_err_t startMeshLinkAP(void)
{
    uint8_t mac[MAC_ADDR_LEN];
    esp_wifi_get_mac(WIFI_IF_AP, mac);
    esp_netif_set_mac(pNetifAP, mac);
    esp_netif_action_start(pNetifAP, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Starts station link over wifi (root node to the router)
 */
static esp_err_t startWifiLinkSta(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_err_t ret;
    void* pDriver = esp_netif_get_io_driver(pNetifSta);
    if ((ret = esp_wifi_register_if_rxcb(pDriver, esp_netif_receive, pNetifSta)) != ESP_OK)
    {
        ESP_LOGE(TAG, "esp_wifi_register_if_rxcb for if=%p failed with %d", pDriver, ret);
        return ESP_FAIL;
    }
    esp_netif_set_mac(pNetifSta, mac);
    esp_netif_action_start(pNetifSta, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Starts station link over mesh (node to root over mesh)
 */
static esp_err_t startMeshLinkSta(void)
{
    uint8_t mac[MAC_ADDR_LEN];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    esp_netif_set_mac(pNetifSta, mac);
    esp_netif_action_start(pNetifSta, NULL, 0, NULL);
    esp_netif_action_connected(pNetifSta, NULL, 0, NULL);
    return ESP_OK;
}

/**
 * @brief Creates esp-netif for AP interface over mesh
 *
 * @return Pointer to esp-netif instance
 */
static esp_netif_t* createMeshLinkAP(void)
{
    esp_netif_inherent_config_t base_cfg = ESP_NETIF_INHERENT_DEFAULT_WIFI_AP()
    ;
    base_cfg.if_desc = "mesh_link_ap";
    base_cfg.ip_info = &g_mesh_netif_subnet_ip;

    esp_netif_config_t netifConfig = { .base = &base_cfg, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_AP };
    esp_netif_t* pNetif = esp_netif_new(&netifConfig);
    assert(pNetif);
    return pNetif;
}

/**
 * @brief Creates esp-netif for station interface over mesh
 *
 * @note Interface needs to be started (later) using the above APIs
 * based on the actual configuration root/node,
 * since root connects normally over wifi
 *
 * @return Pointer to esp-netif instance
 */
static esp_netif_t* createMeshLinkSta(void)
{
    esp_netif_inherent_config_t baseConfig = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    baseConfig.if_desc = "mesh_link_sta";

    esp_netif_config_t netifConfig = { .base = &baseConfig, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA };
    esp_netif_t* pNetif = esp_netif_new(&netifConfig);
    assert(pNetif);
    return pNetif;
}

esp_err_t meshNetifStartRootAP(bool isRoot, uint32_t addr)
{
    if (isRoot)
    {
        if (pNetifAP)
        {
            esp_netif_action_disconnected(pNetifAP, NULL, 0, NULL);
            MeshDeleteIfDriver(esp_netif_get_io_driver(pNetifAP));
            esp_netif_destroy(pNetifAP);
            pNetifAP = NULL;
        }
        pNetifAP = createMeshLinkAP();
        meshNetifDriver* driver = MeshCreateIfDriver(true, true);
        if (driver == NULL)
        {
            ESP_LOGE(TAG, "Failed to create wifi interface handle");
            return ESP_FAIL;
        }
        esp_netif_attach(pNetifAP, driver);
        setDhcpsDNS(pNetifAP, addr);
        startMeshLinkAP();
        ip_napt_enable(g_mesh_netif_subnet_ip.ip.addr, 1);
    }
    return ESP_OK;
}

esp_err_t meshNetifsStart(bool is_root)
{
    if (is_root)
    {
// ROOT: need both sta should use standard wifi, AP mesh link netif

// Root: Station
        if (pNetifSta && strcmp(esp_netif_get_desc(pNetifSta), "sta") == 0)
        {
            ESP_LOGI(TAG, "Already wifi station, no need to do anything");
        }
        else if (pNetifSta && strcmp(esp_netif_get_desc(pNetifSta), "mesh_link_sta") == 0)
        {
            esp_netif_action_disconnected(pNetifSta, NULL, 0, NULL);
            MeshDeleteIfDriver(esp_netif_get_io_driver(pNetifSta));
            esp_netif_destroy(pNetifSta);
            meshNetifInitStation();
        }
        else if (pNetifSta == NULL)
        {
            meshNetifInitStation();
        }
    }
    else
    {
// NODE: create only STA in form of mesh link
        if (pNetifSta && strcmp(esp_netif_get_desc(pNetifSta), "mesh_link_sta") == 0)
        {
            ESP_LOGI(TAG, "Already mesh link station, no need to do anything");
            return ESP_OK;
        }
        if (pNetifSta)
        {
            esp_netif_action_disconnected(pNetifSta, NULL, 0, NULL);
// should remove the actual driver
            if (strcmp(esp_netif_get_desc(pNetifSta), "sta") == 0)
            {
                ESP_LOGI(TAG, "It was a wifi station removing stuff");
                esp_wifi_clear_default_wifi_driver_and_handlers(pNetifSta);
            }
            esp_netif_destroy(pNetifSta);

        }
        pNetifSta = createMeshLinkSta();
// now we create a mesh driver and attach it to the existing netif
        meshNetifDriver* pDriver = MeshCreateIfDriver(false, false);
        if (pDriver == NULL)
        {
            ESP_LOGE(TAG, "Failed to create wifi interface handle");
            return ESP_FAIL;
        }
        esp_netif_attach(pNetifSta, pDriver);
        startMeshLinkSta();
// If we have a AP on NODE -> stop and remove it!
        if (pNetifAP)
        {
            esp_netif_action_disconnected(pNetifAP, NULL, 0, NULL);
            MeshDeleteIfDriver(esp_netif_get_io_driver(pNetifAP));
            esp_netif_destroy(pNetifAP);
            pNetifAP = NULL;
        }

    }
    return ESP_OK;
}

esp_err_t meshNetifsStop(void)
{
    if (pNetifSta && strcmp(esp_netif_get_desc(pNetifSta), "sta") == 0 && pNetifAP == NULL)
    {
        return ESP_OK;
    }

    if (pNetifSta)
    {
        if (strcmp(esp_netif_get_desc(pNetifSta), "sta") == 0)
        {
            esp_netif_action_disconnected(pNetifSta, NULL, 0, NULL);
            esp_netif_action_stop(pNetifSta, NULL, 0, NULL);
            esp_wifi_clear_default_wifi_driver_and_handlers(pNetifSta);
        }
        else
        {
            esp_netif_action_disconnected(pNetifSta, NULL, 0, NULL);
            MeshDeleteIfDriver(esp_netif_get_io_driver(pNetifSta));
        }
        esp_netif_destroy(pNetifSta);
        pNetifSta = NULL;
    }

    if (pNetifAP)
    {
        esp_netif_action_disconnected(pNetifAP, NULL, 0, NULL);
        MeshDeleteIfDriver(esp_netif_get_io_driver(pNetifAP));
        esp_netif_destroy(pNetifAP);
        pNetifAP = NULL;
    }
// reserve the default (STA gets ready to become root)
    meshNetifInitStation();
    startWifiLinkSta();
    return ESP_OK;
}

uint8_t* meshNetifGetStationMAC(void)
{
    meshNetifDriver* pMesh = esp_netif_get_io_driver(pNetifSta);
    return pMesh->sta_mac_addr;
}
