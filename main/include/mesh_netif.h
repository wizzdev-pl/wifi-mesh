#ifndef MESH_NETIF_H_
#define MESH_NETIF_H_

#include "esp_mesh.h"

/*******************************************************
 *                Macros
 *******************************************************/
#define MAC_ADDR_LEN (6u)
#define MAC_ADDR_EQUAL(a, b) (0 == memcmp(a, b, MAC_ADDR_LEN))

/*******************************************************
 *                Type Definitions
 *******************************************************/
typedef void (mesh_raw_recv_cb_t)(mesh_addr_t* pFrom, mesh_data_t* pData);

/*******************************************************
 *                Function Declarations
 *******************************************************/

/**
 * @brief Initializes netifs in a default way before knowing if we are going to be a root
 *
 * @param cb callback receive function for mesh raw packets
 *
 * @return ESP_OK on success
 */
esp_err_t meshNetifsInit(mesh_raw_recv_cb_t* pCb);

/**
 * @brief Destroy the netifs and related structures
 *
 * @return ESP_OK on success
 */
esp_err_t meshNetifsDestroy(void);

/**
 * @brief Start the mesh netifs based on the configuration (root/node)
 *
 * @return ESP_OK on success
 */
esp_err_t meshNetifsStart(bool isRoot);

/**
 * @brief Stop the netifs and reset to the default mode
 *
 * @return ESP_OK on success
 */
esp_err_t meshNetifsStop(void);

/**
 * @brief Start the netif for root AP
 *
 * Note: The AP netif needs to be started separately after root received
 * an IP address from the router so the DNS address could be used for dhcps
 *
 * @param is_root must be true, ignored otherwise
 * @param dns_addr DNS address to use in DHCP server running on roots AP
 *
 * @return ESP_OK on success
 */
esp_err_t meshNetifStartRootAP(bool isRoot, uint32_t DNS_Addr);

/**
 * @brief Returns MAC address of the station interface
 *
 * Used mainly for checking node addresses of the peers in routing table
 * to avoid sending data to oneself
 *
 * @return Pointer to MAC address
 */
uint8_t* meshNetifGetStationMAC(void);

#endif // MESH_NETIF_H_

