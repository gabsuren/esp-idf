# Phase 1 Implementation Guide
## ESP-IDF Transport Layer Refactoring

**Date:** 2025-10-27  
**Phase:** 1 - Foundation (CRITICAL)  
**Duration:** 2 weeks  
**Goal:** Fix Issues #1, #2, #4 (Resource Ownership, Cleanup, Parent Chain)

---

## Table of Contents

1. [Overview](#overview)
2. [Solution 1: Ownership Model](#solution-1-ownership-model)
3. [Solution 2: Resource Management](#solution-2-resource-management)
4. [Solution 4: Chain Operations](#solution-4-chain-operations)
5. [Integration Steps](#integration-steps)
6. [Testing Strategy](#testing-strategy)
7. [Migration Guide](#migration-guide)

---

## Overview

### What's Being Fixed

| Issue | Severity | Problem | Solution |
|-------|----------|---------|----------|
| #1 | CRITICAL | Ambiguous ownership → memory leaks & double-frees | Explicit ownership model |
| #2 | HIGH | Resources not freed in `close()` → 424B leak/reconnect | Resource management system |
| #4 | HIGH | Manual parent chain → forgotten cleanup | Automatic chain operations |

### API Changes

**✅ Backward Compatible** - All existing code continues to work  
**✅ Non-Breaking** - New APIs use `_ex` suffix  
**✅ Opt-In** - Old APIs remain unchanged

---

## Solution 1: Ownership Model

### Problem Statement

**Current Code (Ambiguous):**
```c
esp_transport_handle_t tcp = esp_transport_tcp_init();
esp_transport_handle_t ws = esp_transport_ws_init(tcp);

esp_transport_list_add(list, tcp, "_tcp");    // Who owns tcp?
esp_transport_list_add(list, ws, "ws");       // Who owns ws?

esp_transport_list_destroy(list);
// ❌ Does this destroy tcp? ws? both? neither?
// ❌ Will ws try to destroy tcp (double-free)?
// ❌ Will tcp leak because ws doesn't destroy it?
```

### Solution: Explicit Ownership

**New API:**
```c
typedef enum {
    ESP_TRANSPORT_OWNERSHIP_NONE,       // Caller owns, list won't destroy
    ESP_TRANSPORT_OWNERSHIP_SHARED,     // Reference counted (future)
    ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE   // List owns, will destroy
} esp_transport_ownership_t;

esp_err_t esp_transport_list_add_ex(
    esp_transport_list_handle_t list,
    esp_transport_handle_t t,
    const char *scheme,
    esp_transport_ownership_t ownership
);
```

**Fixed Code:**
```c
esp_transport_handle_t tcp = esp_transport_tcp_init();
esp_transport_handle_t ws = esp_transport_ws_init(tcp);

// ✅ Explicit: List doesn't own tcp (ws owns it via parent chain)
esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);

// ✅ Explicit: List owns ws (will destroy it)
esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);

esp_transport_list_destroy(list);
// ✅ List destroys ws (EXCLUSIVE ownership)
// ✅ ws destroys tcp via parent chain (automatic in Solution 4)
// ✅ tcp NOT destroyed by list (NONE ownership)
// ✅ No double-free! No leak!
```

### Implementation

**Header Addition (esp_transport.h):**
```c
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
```

**Internal Structure Update (esp_transport_internal.h):**
```c
struct esp_transport_item_t {
    int             port;
    char            *scheme;
    void            *data;
    connect_func    _connect;
    io_read_func    _read;
    io_func         _write;
    trans_func      _close;
    poll_func       _poll_read;
    poll_func       _poll_write;
    trans_func      _destroy;
    connect_async_func _connect_async;
    payload_transfer_func  _parent_transfer;
    get_socket_func        _get_socket;
    esp_transport_keep_alive_t *keep_alive_cfg;
    struct esp_foundation_transport *foundation;
    
    // NEW: Ownership field
    esp_transport_ownership_t ownership;  /*!< Ownership semantics for this transport */
    
    STAILQ_ENTRY(esp_transport_item_t) next;
};
```

**Implementation (transport.c):**
```c
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

// Backward compatible wrapper
esp_err_t esp_transport_list_add(
    esp_transport_list_handle_t h,
    esp_transport_handle_t t,
    const char *scheme)
{
    // Default to EXCLUSIVE ownership (backward compatible behavior)
    return esp_transport_list_add_ex(h, t, scheme, ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
}

// Updated destroy to respect ownership
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
```

---

## Solution 2: Resource Management

### Problem Statement

**Current Code (Resources Leak):**
```c
static int ws_connect(esp_transport_handle_t t, ...)
{
    ws->buffer = malloc(2048);  // ⚠️ Allocated here
    ws->redir_host = strdup(location);  // ⚠️ Allocated here
    // ... use connection ...
}

static int ws_close(esp_transport_handle_t t)
{
    // ❌ buffer NOT freed
    // ❌ redir_host NOT freed
    return esp_transport_close(ws->parent);
}

// Resources only freed in destroy() (too late!)
static esp_err_t ws_destroy(esp_transport_handle_t t)
{
    free(ws->buffer);  // ✅ Finally freed, but only on destroy
    free(ws->redir_host);
}

// Result: 424 bytes leaked per reconnection!
```

### Solution: Structured Resource Management

**New API:**
```c
typedef struct transport_resource {
    void **handle_ptr;                              // Pointer to resource pointer
    const char *name;                               // Resource name (for debugging)
    esp_err_t (*init)(void **handle, void *config); // Initialize/allocate resource
    void (*cleanup)(void **handle);                 // Cleanup/free resource
    bool initialized;                               // Track initialization state
} transport_resource_t;

// Initialize all resources in array (stops at NULL sentinel)
esp_err_t transport_resources_init(transport_resource_t *resources, void *config);

// Cleanup all initialized resources (idempotent, safe to call multiple times)
void transport_resources_cleanup(transport_resource_t *resources);

// Helper macro for defining resources
#define TRANSPORT_RESOURCE(ptr, init_fn, cleanup_fn) \
    { .handle_ptr = (void **)&(ptr), \
      .name = #ptr, \
      .init = (init_fn), \
      .cleanup = (cleanup_fn), \
      .initialized = false }
```

**Fixed Code:**
```c
// Define resource handlers
static esp_err_t ws_buffer_init(void **handle, void *config)
{
    *handle = malloc(WS_BUFFER_SIZE);
    return (*handle != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void ws_buffer_cleanup(void **handle)
{
    if (*handle) {
        free(*handle);
        *handle = NULL;
    }
}

static void ws_string_cleanup(void **handle)
{
    if (*handle) {
        free(*handle);
        *handle = NULL;
    }
}

// WebSocket context with resources
typedef struct transport_ws {
    esp_transport_handle_t parent;
    
    // Session resources
    char *buffer;
    char *redir_host;
    
    // Configuration (persistent)
    char *path;
    char *sub_protocol;
    
    // NEW: Resource tracking
    transport_resource_t resources[4];  // +1 for sentinel
} transport_ws_t;

// Initialize resources array once (in init)
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent)
{
    transport_ws_t *ws = calloc(1, sizeof(transport_ws_t));
    ws->parent = parent;
    
    // ✅ Define resources (not allocated yet)
    ws->resources[0] = TRANSPORT_RESOURCE(ws->buffer, ws_buffer_init, ws_buffer_cleanup);
    ws->resources[1] = TRANSPORT_RESOURCE(ws->redir_host, NULL, ws_string_cleanup);
    ws->resources[2] = TRANSPORT_RESOURCE(ws->path, NULL, ws_string_cleanup);
    ws->resources[3] = (transport_resource_t){0};  // Sentinel
    
    return t;
}

// Allocate session resources in connect()
static int ws_connect(esp_transport_handle_t t, ...)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    
    // ✅ Initialize session resources
    if (transport_resources_init(ws->resources, NULL) != ESP_OK) {
        return -1;
    }
    
    // Now buffer and redir_host are allocated and tracked
    // ... HTTP upgrade ...
    
    return 0;
}

// ✅ FREE session resources in close()
static int ws_close(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    
    // ✅ Cleanup session resources (buffer, redir_host)
    transport_resources_cleanup(ws->resources);
    
    return esp_transport_close(ws->parent);
}

// ✅ Idempotent cleanup in destroy() (safe to call even if already cleaned)
static esp_err_t ws_destroy(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    
    // ✅ Cleanup any remaining resources (idempotent)
    transport_resources_cleanup(ws->resources);
    
    free(ws);
    return ESP_OK;
}
```

### Implementation

**Header (esp_transport_resources.h) - NEW FILE:**
```c
/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ESP_TRANSPORT_RESOURCES_H_
#define _ESP_TRANSPORT_RESOURCES_H_

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Resource initialization function signature
 *
 * @param[out] handle  Pointer to resource handle (will be set to allocated resource)
 * @param[in]  config  Configuration data (transport-specific)
 *
 * @return
 *     - ESP_OK on success
 *     - ESP_ERR_NO_MEM if allocation fails
 *     - ESP_FAIL for other errors
 */
typedef esp_err_t (*transport_resource_init_fn)(void **handle, void *config);

/**
 * @brief Resource cleanup function signature
 *
 * @param[in,out] handle  Pointer to resource handle (will be set to NULL after cleanup)
 *
 * @note This function must be idempotent (safe to call multiple times)
 */
typedef void (*transport_resource_cleanup_fn)(void **handle);

/**
 * @brief Transport resource descriptor
 *
 * Describes a single resource (buffer, string, etc.) managed by transport.
 * Resources are automatically tracked and cleaned up.
 */
typedef struct transport_resource {
    void **handle_ptr;                      /*!< Pointer to resource handle */
    const char *name;                       /*!< Resource name (for debugging/logging) */
    transport_resource_init_fn init;        /*!< Initialization function (NULL if manual init) */
    transport_resource_cleanup_fn cleanup;  /*!< Cleanup function (required) */
    bool initialized;                       /*!< Track initialization state */
} transport_resource_t;

/**
 * @brief Helper macro for defining resource descriptors
 *
 * @param ptr       Resource pointer variable
 * @param init_fn   Initialization function (or NULL)
 * @param cleanup_fn Cleanup function (required)
 *
 * Example:
 * @code{c}
 * char *buffer;
 * transport_resource_t resources[] = {
 *     TRANSPORT_RESOURCE(buffer, buffer_init, buffer_cleanup),
 *     {0}  // Sentinel
 * };
 * @endcode
 */
#define TRANSPORT_RESOURCE(ptr, init_fn, cleanup_fn) \
    { .handle_ptr = (void **)&(ptr), \
      .name = #ptr, \
      .init = (init_fn), \
      .cleanup = (cleanup_fn), \
      .initialized = false }

/**
 * @brief Initialize all resources in array
 *
 * Iterates through resource array (until NULL sentinel) and calls init function
 * for each resource. If any initialization fails, all previously initialized
 * resources are cleaned up.
 *
 * @param[in,out] resources  NULL-terminated array of resource descriptors
 * @param[in]     config     Configuration data passed to init functions
 *
 * @return
 *     - ESP_OK if all resources initialized successfully
 *     - ESP_ERR_INVALID_ARG if resources is NULL
 *     - ESP_ERR_NO_MEM if any allocation fails
 *     - ESP_FAIL for other errors
 *
 * @note Resources with NULL init function are skipped
 * @note If initialization fails, cleanup is automatic (no leak)
 *
 * Example:
 * @code{c}
 * if (transport_resources_init(ws->resources, NULL) != ESP_OK) {
 *     ESP_LOGE(TAG, "Failed to initialize resources");
 *     return ESP_ERR_NO_MEM;
 * }
 * @endcode
 */
esp_err_t transport_resources_init(transport_resource_t *resources, void *config);

/**
 * @brief Cleanup all initialized resources
 *
 * Iterates through resource array and calls cleanup function for each
 * initialized resource. After cleanup, sets initialized flag to false.
 *
 * @param[in,out] resources  NULL-terminated array of resource descriptors
 *
 * @note This function is idempotent (safe to call multiple times)
 * @note Only resources with initialized=true are cleaned up
 * @note Resources with NULL cleanup function are skipped
 *
 * Example:
 * @code{c}
 * // In close():
 * transport_resources_cleanup(ws->resources);
 * 
 * // In destroy() (safe to call again):
 * transport_resources_cleanup(ws->resources);  // No-op if already cleaned
 * @endcode
 */
void transport_resources_cleanup(transport_resource_t *resources);

#ifdef __cplusplus
}
#endif

#endif /* _ESP_TRANSPORT_RESOURCES_H_ */
```

**Implementation (transport_resources.c) - NEW FILE:**
```c
/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "esp_log.h"
#include "esp_transport_resources.h"

static const char *TAG = "transport_resources";

esp_err_t transport_resources_init(transport_resource_t *resources, void *config)
{
    if (resources == NULL) {
        ESP_LOGE(TAG, "resources is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    
    // Iterate through resources (until NULL sentinel)
    for (int i = 0; resources[i].handle_ptr != NULL; i++) {
        transport_resource_t *res = &resources[i];
        
        // Skip if already initialized
        if (res->initialized) {
            ESP_LOGD(TAG, "Resource '%s' already initialized, skipping", 
                     res->name ? res->name : "unknown");
            continue;
        }
        
        // Skip if no init function (manual initialization)
        if (res->init == NULL) {
            ESP_LOGD(TAG, "Resource '%s' has no init function, skipping", 
                     res->name ? res->name : "unknown");
            continue;
        }
        
        // Initialize resource
        ESP_LOGD(TAG, "Initializing resource '%s'", res->name ? res->name : "unknown");
        esp_err_t err = res->init(res->handle_ptr, config);
        
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize resource '%s': %d", 
                     res->name ? res->name : "unknown", err);
            
            // Cleanup all previously initialized resources
            transport_resources_cleanup(resources);
            return err;
        }
        
        // Mark as initialized
        res->initialized = true;
        ESP_LOGD(TAG, "Resource '%s' initialized successfully", 
                 res->name ? res->name : "unknown");
    }
    
    return ESP_OK;
}

void transport_resources_cleanup(transport_resource_t *resources)
{
    if (resources == NULL) {
        return;
    }
    
    // Iterate through resources (until NULL sentinel)
    for (int i = 0; resources[i].handle_ptr != NULL; i++) {
        transport_resource_t *res = &resources[i];
        
        // Skip if not initialized
        if (!res->initialized) {
            continue;
        }
        
        // Skip if no cleanup function
        if (res->cleanup == NULL) {
            ESP_LOGW(TAG, "Resource '%s' has no cleanup function", 
                     res->name ? res->name : "unknown");
            res->initialized = false;
            continue;
        }
        
        // Cleanup resource
        ESP_LOGD(TAG, "Cleaning up resource '%s'", res->name ? res->name : "unknown");
        res->cleanup(res->handle_ptr);
        
        // Mark as not initialized (idempotent)
        res->initialized = false;
    }
}
```

---

## Solution 4: Chain Operations

### Problem Statement

**Current Code (Manual Parent Management):**
```c
static int ws_close(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    // ✅ Remembers to close parent
    return esp_transport_close(ws->parent);
}

static esp_err_t ws_destroy(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    free(ws->buffer);
    free(ws);
    // ❌ FORGOT to destroy parent!
    // Missing: esp_transport_destroy(ws->parent);
    return ESP_OK;
}
```

### Solution: Automatic Chain Operations

**New API:**
```c
// Execute operation on entire transport chain (transport + all parents)
esp_err_t esp_transport_chain_execute(
    esp_transport_handle_t t,
    esp_err_t (*op)(esp_transport_handle_t),
    bool aggregate_errors
);

// Convenience wrappers
esp_err_t esp_transport_close_chain(esp_transport_handle_t t);
esp_err_t esp_transport_destroy_chain(esp_transport_handle_t t);
```

**Fixed Code:**
```c
// Transport implementation no longer needs parent logic
static esp_err_t ws_destroy(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    free(ws->buffer);
    free(ws);
    // ✅ No parent cleanup needed - automatic!
    return ESP_OK;
}

// Application code
esp_websocket_client_destroy(client);
    // → calls esp_transport_destroy_chain(ws)
    //   → ws_destroy(ws)
    //   → automatically: esp_transport_destroy_chain(tcp)
    //     → tcp_destroy(tcp)
    // ✅ Entire chain cleaned up automatically!
```

### Implementation

**Header Addition (esp_transport.h):**
```c
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
esp_err_t esp_transport_destroy_chain(esp_transport_handle_t t);

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
```

**Internal Structure Update (esp_transport_internal.h):**
```c
struct esp_transport_item_t {
    // ... existing fields ...
    
    // NEW: Parent transport (for chain operations)
    esp_transport_handle_t parent;  /*!< Parent transport in chain (NULL if base) */
    
    STAILQ_ENTRY(esp_transport_item_t) next;
};
```

**Implementation (transport.c):**
```c
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

// Wrapper for close
static esp_err_t transport_close_wrapper(esp_transport_handle_t t)
{
    if (t && t->_close) {
        int ret = t->_close(t);
        return (ret == 0) ? ESP_OK : ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t esp_transport_close_chain(esp_transport_handle_t t)
{
    return esp_transport_chain_execute(t, transport_close_wrapper, false);
}

// Wrapper for destroy
static esp_err_t transport_destroy_wrapper(esp_transport_handle_t t)
{
    if (t && t->_destroy) {
        return t->_destroy(t);
    }
    return ESP_OK;
}

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

esp_transport_handle_t esp_transport_get_parent(esp_transport_handle_t t)
{
    return t ? t->parent : NULL;
}
```

**Transport-Specific Update (transport_ws.c):**
```c
esp_transport_handle_t esp_transport_ws_init(esp_transport_handle_t parent_handle)
{
    if (parent_handle == NULL) {
        ESP_LOGE(TAG, "Invalid parent protocol");
        return NULL;
    }
    
    esp_transport_handle_t t = esp_transport_init();
    transport_ws_t *ws = calloc(1, sizeof(transport_ws_t));
    
    ws->parent = parent_handle;
    t->foundation = parent_handle->foundation;
    
    // ✅ NEW: Set parent in transport structure for chain operations
    t->parent = parent_handle;
    
    // ... rest of initialization ...
    
    return t;
}
```

---

## Integration Steps

### Step 1: Update Header Files

1. **esp_transport.h** - Add new APIs (ownership, chain ops)
2. **esp_transport_internal.h** - Add ownership and parent fields to struct
3. **esp_transport_resources.h** - NEW FILE for resource management

### Step 2: Update Implementation Files

1. **transport.c**:
   - Add `esp_transport_list_add_ex()`
   - Update `esp_transport_list_clean()` to respect ownership
   - Add chain operation functions

2. **transport_resources.c** - NEW FILE:
   - Implement resource init/cleanup

### Step 3: Update Transport Implementations

**transport_ws.c:**
```c
// In init:
t->parent = parent_handle;  // Set parent for chain ops
ws->resources[...] = ...;   // Define resources

// In connect:
transport_resources_init(ws->resources, NULL);  // Allocate session resources

// In close:
transport_resources_cleanup(ws->resources);  // Free session resources
return esp_transport_close(ws->parent);  // Or use chain API

// In destroy:
transport_resources_cleanup(ws->resources);  // Idempotent cleanup
// parent destroy is automatic via chain
```

**transport_ssl.c, transport_tcp.c:**
- Similar resource management pattern
- Set parent in init (if applicable)

### Step 4: Update Application Code

**esp_websocket_client.c:**
```c
static esp_err_t esp_websocket_client_create_transport(...)
{
    // ... create transports ...
    
    // ✅ Use new ownership API
    esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
    esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
}

static esp_err_t esp_websocket_client_destroy(...)
{
    // List destroy now respects ownership
    esp_transport_list_destroy(client->transport_list);
    // ✅ ws destroyed (EXCLUSIVE), tcp destroyed via chain
}
```

### Step 5: Add CMake Build Rules

**components/tcp_transport/CMakeLists.txt:**
```cmake
idf_component_register(
    SRCS
        "transport.c"
        "transport_ssl.c"
        "transport_tcp.c"
        "transport_ws.c"
        "transport_resources.c"  # NEW
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    REQUIRES
        esp-tls
        esp_timer
)
```

---

## Testing Strategy

### Unit Tests

**test_transport_ownership.c:**
```c
TEST_CASE("Transport ownership - EXCLUSIVE", "[transport]")
{
    esp_transport_list_handle_t list = esp_transport_list_init();
    esp_transport_handle_t t1 = esp_transport_init();
    
    esp_transport_list_add_ex(list, t1, "test", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
    
    // Destroy list should destroy transport
    esp_transport_list_destroy(list);
    
    // ✅ No leak, no crash
}

TEST_CASE("Transport ownership - NONE", "[transport]")
{
    esp_transport_list_handle_t list = esp_transport_list_init();
    esp_transport_handle_t t1 = esp_transport_init();
    
    esp_transport_list_add_ex(list, t1, "test", ESP_TRANSPORT_OWNERSHIP_NONE);
    
    // Destroy list should NOT destroy transport
    esp_transport_list_destroy(list);
    
    // Manual cleanup
    esp_transport_destroy(t1);
    
    // ✅ No crash
}
```

**test_transport_resources.c:**
```c
TEST_CASE("Resource management - init/cleanup", "[transport]")
{
    char *buffer = NULL;
    
    transport_resource_t resources[] = {
        TRANSPORT_RESOURCE(buffer, test_buffer_init, test_buffer_cleanup),
        {0}
    };
    
    // Init
    TEST_ASSERT_EQUAL(ESP_OK, transport_resources_init(resources, NULL));
    TEST_ASSERT_NOT_NULL(buffer);
    TEST_ASSERT_TRUE(resources[0].initialized);
    
    // Cleanup
    transport_resources_cleanup(resources);
    TEST_ASSERT_NULL(buffer);
    TEST_ASSERT_FALSE(resources[0].initialized);
    
    // Idempotent
    transport_resources_cleanup(resources);  // No crash
}
```

**test_transport_chain.c:**
```c
TEST_CASE("Chain operations - destroy chain", "[transport]")
{
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    
    // Destroy chain should destroy both
    esp_transport_destroy_chain(ws);
    
    // ✅ No leak (verify with Valgrind)
}
```

### Memory Leak Tests

**Run with Valgrind:**
```bash
cd components/tcp_transport/test
idf.py build
valgrind --leak-check=full --show-leak-kinds=all ./build/test_transport

# Expected output:
# ==12345== HEAP SUMMARY:
# ==12345==     in use at exit: 0 bytes in 0 blocks
# ==12345==   total heap usage: 100 allocs, 100 frees, 10,240 bytes allocated
# ==12345== 
# ==12345== All heap blocks were freed -- no leaks are possible
```

### Integration Tests

**test_websocket_reconnect.c:**
```c
TEST_CASE("WebSocket reconnection - no leak", "[websocket]")
{
    esp_websocket_client_config_t config = {
        .uri = "ws://localhost:8080/ws"
    };
    
    esp_websocket_client_handle_t client = esp_websocket_client_init(&config);
    
    // Reconnect 100 times
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_EQUAL(ESP_OK, esp_websocket_client_start(client));
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_websocket_client_stop(client);
    }
    
    esp_websocket_client_destroy(client);
    
    // ✅ Check heap before/after
    // Expected: 0 bytes leaked (was 424 bytes/reconnect = 42.4 KB leak)
}
```

---

## Migration Guide

### For Transport Developers

**Before:**
```c
static esp_err_t my_transport_destroy(esp_transport_handle_t t)
{
    my_transport_t *ctx = esp_transport_get_context_data(t);
    
    free(ctx->buffer);
    free(ctx->hostname);
    free(ctx);
    
    // ❌ Must remember to destroy parent
    // esp_transport_destroy(ctx->parent);  // Often forgotten!
    
    return ESP_OK;
}
```

**After:**
```c
static esp_err_t my_transport_destroy(esp_transport_handle_t t)
{
    my_transport_t *ctx = esp_transport_get_context_data(t);
    
    // ✅ Use resource management
    transport_resources_cleanup(ctx->resources);
    
    free(ctx);
    
    // ✅ Parent destroy is automatic (via chain)
    
    return ESP_OK;
}
```

### For Application Developers

**Before:**
```c
esp_transport_list_add(list, tcp, "_tcp");
esp_transport_list_add(list, ws, "ws");

// ❌ Ambiguous: Does list own tcp? Does ws own tcp? Both?
```

**After:**
```c
esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);

// ✅ Clear: List doesn't own tcp, list owns ws, ws owns tcp (via parent)
```

### Backward Compatibility

**Old code continues to work:**
```c
// Old API (still works)
esp_transport_list_add(list, t, "scheme");
// → Calls esp_transport_list_add_ex(list, t, "scheme", OWNERSHIP_EXCLUSIVE)

esp_transport_destroy(t);
// → Still works for single transports

esp_transport_close(t);
// → Still works
```

**New code uses new APIs:**
```c
// New API (explicit)
esp_transport_list_add_ex(list, t, "scheme", OWNERSHIP_NONE);

esp_transport_destroy_chain(t);  // For chained transports

esp_transport_close_chain(t);  // For chained transports
```

---

## Success Metrics

### Phase 1 Completion Checklist

- [ ] All new APIs implemented and tested
- [ ] Unit tests pass (ownership, resources, chain)
- [ ] Memory leak tests pass (Valgrind: 0 leaks)
- [ ] Integration tests pass (WebSocket reconnect)
- [ ] Documentation updated
- [ ] Backward compatibility verified

### Expected Results

| Metric | Before | After | Target |
|--------|--------|-------|--------|
| Memory leak (per reconnect) | 424 bytes | 0 bytes | ✅ 0 bytes |
| Double-free errors | Possible | None | ✅ 0 errors |
| Use-after-free errors | Possible | None | ✅ 0 errors |
| Parent cleanup bugs | 1 (ws_destroy) | 0 | ✅ 0 bugs |
| Ownership clarity | Ambiguous | Explicit | ✅ Clear |

### Validation Commands

```bash
# Build with AddressSanitizer
idf.py -DCONFIG_COMPILER_ASAN=y build

# Run tests
idf.py test

# Memory leak check
valgrind --leak-check=full --show-leak-kinds=all ./build/test_transport

# Reconnection stress test
./test_websocket_reconnect 100

# Check metrics
heap_caps_get_free_size(MALLOC_CAP_DEFAULT)  # Should be stable
```

---

## Timeline

| Week | Task | Deliverable |
|------|------|-------------|
| Week 1, Days 1-2 | Implement Solution 1 (Ownership) | APIs + tests |
| Week 1, Days 3-4 | Implement Solution 2 (Resources) | APIs + tests |
| Week 1, Day 5 | Implement Solution 4 (Chain) | APIs + tests |
| Week 2, Days 1-2 | Integration (apply to transports) | Updated transport_ws.c |
| Week 2, Days 3-4 | Testing & validation | All tests pass |
| Week 2, Day 5 | Documentation & review | Ready for merge |

---

## Next Steps

After Phase 1 completion:
1. **Phase 2**: Apply to all transports + simple state tracking
2. **Phase 3**: Memory pooling optimization
3. **Phase 4-5**: Complete migration + polish

---

**Document Version:** 1.0  
**Status:** Implementation Ready ✅  
**Review Required:** Yes (before merge)

