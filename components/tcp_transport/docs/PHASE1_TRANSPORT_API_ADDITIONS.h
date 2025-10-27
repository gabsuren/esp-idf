/*
 * PHASE 1 API ADDITIONS FOR esp_transport.h
 * 
 * Add these declarations to components/tcp_transport/include/esp_transport.h
 */

/**
 * @brief Transport ownership semantics for list management
 */
typedef enum {
    ESP_TRANSPORT_OWNERSHIP_NONE = 0,       /*!< Caller owns transport, list won't destroy it */
    ESP_TRANSPORT_OWNERSHIP_SHARED = 1,     /*!< Shared ownership (reference counted - future) */
    ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE = 2   /*!< List owns transport exclusively, will destroy on cleanup */
} esp_transport_ownership_t;

/**
 * @brief Add transport to list with explicit ownership semantics
 *
 * This function adds a transport to the list with explicit ownership control.
 * Ownership determines whether esp_transport_list_destroy() will destroy this transport.
 *
 * @param[in]  list       The transport list handle
 * @param[in]  t          The transport handle to add
 * @param[in]  scheme     The scheme name (e.g., "ws", "wss", "tcp")
 * @param[in]  ownership  Ownership semantics (NONE, SHARED, EXCLUSIVE)
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_INVALID_ARG if list or transport is NULL
 *     - ESP_ERR_NO_MEM if allocation fails
 *
 * @note The old esp_transport_list_add() now defaults to EXCLUSIVE ownership
 *       for backward compatibility
 *
 * Example:
 * @code{c}
 * esp_transport_handle_t tcp = esp_transport_tcp_init();
 * esp_transport_handle_t ws = esp_transport_ws_init(tcp);
 * 
 * // tcp is owned by ws (parent chain), not by list
 * esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
 * 
 * // ws is owned by list, will be destroyed with list
 * esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
 * @endcode
 */
esp_err_t esp_transport_list_add_ex(
    esp_transport_list_handle_t list,
    esp_transport_handle_t t,
    const char *scheme,
    esp_transport_ownership_t ownership
);

/**
 * @brief Execute operation on transport chain
 *
 * Executes the provided operation on the transport and recursively on all
 * parent transports in the chain. Useful for operations that need to affect
 * the entire transport stack (e.g., close, destroy).
 *
 * @param[in] t                 The transport handle (top of chain)
 * @param[in] op                Operation function to execute on each transport
 * @param[in] aggregate_errors  If true, collect all errors; if false, stop on first error
 *
 * @return
 *     - ESP_OK if all operations succeeded
 *     - First error encountered (if aggregate_errors=false)
 *     - Last error encountered (if aggregate_errors=true)
 *
 * @note The operation is called on transport first, then recursively on parent
 *
 * Example:
 * @code{c}
 * esp_err_t my_close(esp_transport_handle_t t) {
 *     // Custom close logic
 *     return ESP_OK;
 * }
 * 
 * // Close entire chain
 * esp_transport_chain_execute(ws_transport, my_close, false);
 * @endcode
 */
esp_err_t esp_transport_chain_execute(
    esp_transport_handle_t t,
    esp_err_t (*op)(esp_transport_handle_t),
    bool aggregate_errors
);

/**
 * @brief Close transport and all parents in chain
 *
 * Convenience function that closes the transport and recursively closes
 * all parent transports. Equivalent to calling esp_transport_close() on
 * each transport in the chain manually.
 *
 * @param[in] t  The transport handle (top of chain)
 *
 * @return
 *     - ESP_OK if all closes succeeded
 *     - First error encountered
 *
 * @note This is a convenience wrapper for esp_transport_chain_execute()
 *
 * Example:
 * @code{c}
 * // Instead of:
 * esp_transport_close(ws);
 * esp_transport_close(ws->parent);  // Manual
 * 
 * // Use:
 * esp_transport_close_chain(ws);  // Automatic!
 * @endcode
 */
esp_err_t esp_transport_close_chain(esp_transport_handle_t t);

/**
 * @brief Destroy transport and all parents in chain
 *
 * Convenience function that destroys the transport and recursively destroys
 * all parent transports. This is the recommended way to destroy chained
 * transports to prevent memory leaks.
 *
 * @param[in] t  The transport handle (top of chain)
 *
 * @return
 *     - ESP_OK if all destroys succeeded
 *     - First error encountered
 *
 * @note This function MUST be used for chained transports to prevent leaks
 * @note After calling this, transport handle is invalid
 *
 * Example:
 * @code{c}
 * esp_transport_handle_t tcp = esp_transport_tcp_init();
 * esp_transport_handle_t ws = esp_transport_ws_init(tcp);
 * 
 * // When done:
 * esp_transport_destroy_chain(ws);  // Destroys both ws and tcp
 * @endcode
 */
esp_transport_destroy_chain(esp_transport_handle_t t);

/**
 * @brief Get parent transport from chain
 *
 * Helper function to get the parent transport from a chained transport.
 * Returns NULL if transport has no parent.
 *
 * @param[in] t  The transport handle
 *
 * @return
 *     - Parent transport handle
 *     - NULL if no parent or transport is NULL
 *
 * @note This is used internally by chain operations
 */
esp_transport_handle_t esp_transport_get_parent(esp_transport_handle_t t);

