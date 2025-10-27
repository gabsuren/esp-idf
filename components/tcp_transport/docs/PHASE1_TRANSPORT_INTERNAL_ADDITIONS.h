/*
 * PHASE 1 INTERNAL STRUCTURE ADDITIONS
 * 
 * Add these fields to struct esp_transport_item_t in:
 * components/tcp_transport/private_include/esp_transport_internal.h
 */

struct esp_transport_item_t {
    int             port;
    char            *scheme;        /*!< Tag name */
    void            *data;          /*!< Additional transport data */
    connect_func    _connect;       /*!< Connect function of this transport */
    io_read_func    _read;          /*!< Read */
    io_func         _write;         /*!< Write */
    trans_func      _close;         /*!< Close */
    poll_func       _poll_read;     /*!< Poll and read */
    poll_func       _poll_write;    /*!< Poll and write */
    trans_func      _destroy;       /*!< Destroy and free transport */
    connect_async_func _connect_async;      /*!< non-blocking connect function of this transport */
    payload_transfer_func  _parent_transfer;        /*!< Function returning underlying transport layer */
    get_socket_func        _get_socket;             /*!< Function returning the transport's socket */
    esp_transport_keep_alive_t *keep_alive_cfg;     /*!< TCP keep-alive config */
    struct esp_foundation_transport *foundation;    /*!< Foundation transport pointer available from each transport */
    
    // ==================== PHASE 1 ADDITIONS ====================
    
    /**
     * @brief Ownership semantics for transport list management
     * 
     * Determines whether esp_transport_list_destroy() will destroy this transport:
     * - ESP_TRANSPORT_OWNERSHIP_NONE: List won't destroy, caller is responsible
     * - ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE: List will destroy on cleanup
     * - ESP_TRANSPORT_OWNERSHIP_SHARED: Reference counted (future)
     */
    esp_transport_ownership_t ownership;
    
    /**
     * @brief Parent transport in chain (NULL if this is base transport)
     * 
     * Used by chain operations (esp_transport_destroy_chain, etc.) to automatically
     * propagate operations up the transport stack. For example:
     * - WebSocket transport has parent = SSL transport
     * - SSL transport has parent = TCP transport
     * - TCP transport has parent = NULL (base)
     */
    esp_transport_handle_t parent;
    
    // ================== END PHASE 1 ADDITIONS ==================

    STAILQ_ENTRY(esp_transport_item_t) next;
};

