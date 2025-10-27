/*
 * PHASE 1 IMPLEMENTATION ADDITIONS FOR transport.c
 * 
 * Add these functions to components/tcp_transport/transport.c
 */

#include <stdlib.h>
#include <string.h>
#include <esp_tls.h>

#include "sys/queue.h"
#include "esp_log.h"

#include "esp_transport_internal.h"
#include "esp_transport.h"

// ==================== SOLUTION 1: OWNERSHIP MODEL ====================

/**
 * @brief Add transport to list with explicit ownership
 */
esp_err_t esp_transport_list_add_ex(
    esp_transport_list_handle_t h,
    esp_transport_handle_t t,
    const char *scheme,
    esp_transport_ownership_t ownership)
{
    if (h == NULL || t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Set ownership
    t->ownership = ownership;
    
    // Allocate and set scheme
    t->scheme = calloc(1, strlen(scheme) + 1);
    ESP_TRANSPORT_MEM_CHECK(TAG, t->scheme, return ESP_ERR_NO_MEM);
    strcpy(t->scheme, scheme);
    
    // Add to list
    STAILQ_INSERT_TAIL(h, t, next);
    
    ESP_LOGD(TAG, "Added transport '%s' with ownership=%d", scheme, ownership);
    return ESP_OK;
}

/**
 * @brief UPDATED: Original esp_transport_list_add now wraps _ex with EXCLUSIVE ownership
 */
esp_err_t esp_transport_list_add(
    esp_transport_list_handle_t h,
    esp_transport_handle_t t,
    const char *scheme)
{
    // Default to EXCLUSIVE ownership (backward compatible behavior)
    return esp_transport_list_add_ex(h, t, scheme, ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
}

/**
 * @brief UPDATED: Respect ownership when cleaning up list
 */
esp_err_t esp_transport_list_clean(esp_transport_list_handle_t h)
{
    if (h == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_transport_handle_t item = STAILQ_FIRST(h);
    esp_transport_handle_t tmp;
    
    while (item != NULL) {
        tmp = STAILQ_NEXT(item, next);
        
        // ✅ NEW: Check ownership before destroying
        if (item->ownership == ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE) {
            ESP_LOGD(TAG, "Destroying transport '%s' (EXCLUSIVE ownership)", 
                     item->scheme ? item->scheme : "unknown");
            esp_transport_destroy(item);
        } else {
            ESP_LOGD(TAG, "Skipping transport '%s' (ownership=%d)", 
                     item->scheme ? item->scheme : "unknown", item->ownership);
            // Just free the scheme, not the transport itself
            if (item->scheme) {
                free(item->scheme);
                item->scheme = NULL;
            }
        }
        
        item = tmp;
    }
    
    STAILQ_INIT(h);
    return ESP_OK;
}

// ==================== SOLUTION 4: CHAIN OPERATIONS ====================

/**
 * @brief Execute operation on transport chain
 */
esp_err_t esp_transport_chain_execute(
    esp_transport_handle_t t,
    esp_err_t (*op)(esp_transport_handle_t),
    bool aggregate_errors)
{
    if (t == NULL || op == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t err = ESP_OK;
    esp_err_t first_error = ESP_OK;
    
    // Execute operation on current transport
    ESP_LOGD(TAG, "Executing chain operation on transport '%s'", 
             t->scheme ? t->scheme : "unknown");
    
    err = op(t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Chain operation failed on '%s': %d", 
                 t->scheme ? t->scheme : "unknown", err);
        if (first_error == ESP_OK) {
            first_error = err;
        }
        if (!aggregate_errors) {
            return first_error;
        }
    }
    
    // Recursively execute on parent (if exists)
    if (t->parent) {
        ESP_LOGD(TAG, "Continuing chain operation to parent");
        esp_err_t parent_err = esp_transport_chain_execute(t->parent, op, aggregate_errors);
        
        if (parent_err != ESP_OK) {
            ESP_LOGW(TAG, "Chain operation failed on parent: %d", parent_err);
            if (first_error == ESP_OK) {
                first_error = parent_err;
            }
            if (aggregate_errors) {
                err = parent_err;  // Keep last error
            }
        }
    }
    
    return aggregate_errors ? err : first_error;
}

/**
 * @brief Wrapper for close operation
 */
static esp_err_t transport_close_wrapper(esp_transport_handle_t t)
{
    if (t && t->_close) {
        int ret = t->_close(t);
        return (ret == 0) ? ESP_OK : ESP_FAIL;
    }
    return ESP_OK;
}

/**
 * @brief Close transport and all parents in chain
 */
esp_err_t esp_transport_close_chain(esp_transport_handle_t t)
{
    return esp_transport_chain_execute(t, transport_close_wrapper, false);
}

/**
 * @brief Destroy transport and all parents in chain
 */
esp_err_t esp_transport_destroy_chain(esp_transport_handle_t t)
{
    if (t == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Get parent before destroying current transport
    esp_transport_handle_t parent = t->parent;
    
    // Destroy current transport
    ESP_LOGD(TAG, "Destroying transport '%s'", t->scheme ? t->scheme : "unknown");
    
    esp_err_t err = ESP_OK;
    if (t->_destroy) {
        err = t->_destroy(t);
    }
    
    // Free scheme
    if (t->scheme) {
        free(t->scheme);
    }
    
    // Free transport structure
    free(t);
    
    // Recursively destroy parent
    if (parent) {
        ESP_LOGD(TAG, "Destroying parent transport");
        esp_err_t parent_err = esp_transport_destroy_chain(parent);
        if (parent_err != ESP_OK && err == ESP_OK) {
            err = parent_err;
        }
    }
    
    return err;
}

/**
 * @brief Get parent transport from chain
 */
esp_transport_handle_t esp_transport_get_parent(esp_transport_handle_t t)
{
    return t ? t->parent : NULL;
}

// ==================== HELPER FUNCTIONS ====================

/**
 * @brief UPDATED: Initialize transport with default values
 * 
 * Update existing esp_transport_init() to initialize new fields
 */
esp_transport_handle_t esp_transport_init(void)
{
    esp_transport_handle_t transport = calloc(1, sizeof(struct esp_transport_item_t));
    ESP_TRANSPORT_MEM_CHECK(TAG, transport, return NULL);
    
    // ✅ NEW: Initialize new fields
    transport->ownership = ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE;  // Default
    transport->parent = NULL;
    
    return transport;
}

