/*
 * PHASE 1 EXAMPLE USAGE
 * 
 * Complete before/after examples showing how to use Phase 1 APIs
 */

#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "esp_transport_ssl.h"
#include "esp_transport_ws.h"
#include "esp_transport_resources.h"
#include "esp_log.h"

static const char *TAG = "transport_example";

// ==================== EXAMPLE 1: OWNERSHIP MODEL ====================

/**
 * BEFORE: Ambiguous ownership
 */
void example_before_ownership(void)
{
    esp_transport_list_handle_t list = esp_transport_list_init();
    
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    
    // ❌ Ambiguous: Who owns tcp? List? ws? Both?
    esp_transport_list_add(list, tcp, "_tcp");
    esp_transport_list_add(list, ws, "ws");
    
    // Later:
    esp_transport_list_destroy(list);
    // ❌ Does this destroy tcp? Will ws also try to destroy tcp (double-free)?
    // ❌ Or will tcp leak because nobody destroys it?
}

/**
 * AFTER: Explicit ownership
 */
void example_after_ownership(void)
{
    esp_transport_list_handle_t list = esp_transport_list_init();
    
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    
    // ✅ Explicit: tcp is owned by ws (via parent chain), list doesn't own it
    esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
    
    // ✅ Explicit: ws is owned by list, will be destroyed with list
    esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
    
    // Later:
    esp_transport_list_destroy(list);
    // ✅ List destroys ws (EXCLUSIVE ownership)
    // ✅ ws destroys tcp via parent chain (automatic in Phase 1)
    // ✅ tcp NOT destroyed by list (NONE ownership)
    // ✅ No double-free! No leak!
}

// ==================== EXAMPLE 2: RESOURCE MANAGEMENT ====================

/**
 * Example resource cleanup handlers
 */
static esp_err_t buffer_init(void **handle, void *config)
{
    size_t size = config ? *(size_t *)config : 2048;
    *handle = malloc(size);
    return (*handle != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static void buffer_cleanup(void **handle)
{
    if (*handle) {
        free(*handle);
        *handle = NULL;
    }
}

static void string_cleanup(void **handle)
{
    if (*handle) {
        free(*handle);
        *handle = NULL;
    }
}

/**
 * BEFORE: Manual resource management (leak-prone)
 */
typedef struct my_transport_before {
    char *buffer;
    char *hostname;
} my_transport_before_t;

static int my_transport_connect_before(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    my_transport_before_t *ctx = esp_transport_get_context_data(t);
    
    // ❌ Allocate resources
    ctx->buffer = malloc(2048);
    ctx->hostname = strdup(host);
    
    // ... connection logic ...
    
    return 0;
}

static int my_transport_close_before(esp_transport_handle_t t)
{
    // ❌ Resources NOT freed here!
    return 0;
}

static esp_err_t my_transport_destroy_before(esp_transport_handle_t t)
{
    my_transport_before_t *ctx = esp_transport_get_context_data(t);
    
    // ✅ Finally freed, but only on destroy (too late for reconnections)
    free(ctx->buffer);
    free(ctx->hostname);
    free(ctx);
    
    return ESP_OK;
}

/**
 * AFTER: Structured resource management (leak-free)
 */
typedef struct my_transport_after {
    char *buffer;
    char *hostname;
    
    // ✅ Resource tracking
    transport_resource_t resources[3];  // +1 for sentinel
} my_transport_after_t;

static esp_transport_handle_t my_transport_init_after(void)
{
    esp_transport_handle_t t = esp_transport_init();
    my_transport_after_t *ctx = calloc(1, sizeof(my_transport_after_t));
    
    // ✅ Define resources (not allocated yet)
    ctx->resources[0] = TRANSPORT_RESOURCE(ctx->buffer, buffer_init, buffer_cleanup);
    ctx->resources[1] = TRANSPORT_RESOURCE(ctx->hostname, NULL, string_cleanup);
    ctx->resources[2] = (transport_resource_t){0};  // Sentinel
    
    esp_transport_set_context_data(t, ctx);
    return t;
}

static int my_transport_connect_after(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    my_transport_after_t *ctx = esp_transport_get_context_data(t);
    
    // ✅ Initialize resources (buffer allocated automatically)
    if (transport_resources_init(ctx->resources, NULL) != ESP_OK) {
        return -1;
    }
    
    // ✅ Manual initialization for hostname (since init=NULL)
    ctx->hostname = strdup(host);
    ctx->resources[1].initialized = true;
    
    // ... connection logic ...
    
    return 0;
}

static int my_transport_close_after(esp_transport_handle_t t)
{
    my_transport_after_t *ctx = esp_transport_get_context_data(t);
    
    // ✅ Free resources immediately on close
    transport_resources_cleanup(ctx->resources);
    
    return 0;
}

static esp_err_t my_transport_destroy_after(esp_transport_handle_t t)
{
    my_transport_after_t *ctx = esp_transport_get_context_data(t);
    
    // ✅ Idempotent cleanup (safe even if already cleaned in close)
    transport_resources_cleanup(ctx->resources);
    
    free(ctx);
    return ESP_OK;
}

// ==================== EXAMPLE 3: CHAIN OPERATIONS ====================

/**
 * BEFORE: Manual parent management (error-prone)
 */
static int ws_close_before(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    
    // ✅ Remembers to close parent
    return esp_transport_close(ws->parent);
}

static esp_err_t ws_destroy_before(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    
    free(ws->buffer);
    free(ws);
    
    // ❌ FORGOT to destroy parent!
    // Missing: esp_transport_destroy(ws->parent);
    
    return ESP_OK;
}

/**
 * AFTER: Automatic chain management
 */
static int ws_close_after(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    
    // Option 1: Still works (manual)
    return esp_transport_close(ws->parent);
    
    // Option 2: Use chain API (recommended for consistency)
    // return esp_transport_close_chain(ws->parent);
}

static esp_err_t ws_destroy_after(esp_transport_handle_t t)
{
    transport_ws_t *ws = esp_transport_get_context_data(t);
    
    free(ws->buffer);
    free(ws);
    
    // ✅ Parent destroy is automatic (via esp_transport_destroy_chain)
    // No need to manually destroy parent!
    
    return ESP_OK;
}

/**
 * Application usage
 */
void example_chain_operations(void)
{
    // Create chain: ws → ssl → tcp
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ssl = esp_transport_ssl_init();
    esp_transport_handle_t ws = esp_transport_ws_init(ssl);
    
    // Set parent relationships (done in init functions)
    // ws->parent = ssl
    // ssl->parent = tcp
    
    // ... use connection ...
    
    // Close entire chain
    esp_transport_close_chain(ws);  // Closes ws → ssl → tcp
    
    // Destroy entire chain
    esp_transport_destroy_chain(ws);  // Destroys ws → ssl → tcp
    // ✅ All three transports cleaned up automatically!
}

// ==================== EXAMPLE 4: COMPLETE INTEGRATION ====================

/**
 * Complete example: WebSocket over SSL with Phase 1 patterns
 */
void example_complete_websocket_client(void)
{
    // 1. Create transport list
    esp_transport_list_handle_t list = esp_transport_list_init();
    
    // 2. Create transport chain for WSS
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ssl = esp_transport_ssl_init();
    
    // Configure SSL
    esp_transport_ssl_set_cert_data(ssl, server_cert_pem_start, 
                                    server_cert_pem_end - server_cert_pem_start);
    
    // 3. Create WebSocket transport
    esp_transport_handle_t wss = esp_transport_ws_init(ssl);
    
    // 4. Add to list with explicit ownership
    esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
    esp_transport_list_add_ex(list, ssl, "_ssl", ESP_TRANSPORT_OWNERSHIP_NONE);
    esp_transport_list_add_ex(list, wss, "wss", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
    
    // ✅ Only wss is owned by list
    // ✅ tcp and ssl are owned by the parent chain
    
    // 5. Use connection
    esp_transport_handle_t transport = esp_transport_list_get_transport(list, "wss");
    
    if (esp_transport_connect(transport, "example.com", 443, 5000) == 0) {
        ESP_LOGI(TAG, "Connected successfully");
        
        // Send/receive data
        const char *data = "Hello WebSocket";
        esp_transport_write(transport, data, strlen(data), 1000);
        
        char buf[128];
        int len = esp_transport_read(transport, buf, sizeof(buf), 1000);
        ESP_LOGI(TAG, "Received %d bytes", len);
        
        // Close connection
        esp_transport_close(transport);
    }
    
    // 6. Cleanup
    esp_transport_list_destroy(list);
    // ✅ List destroys wss (EXCLUSIVE ownership)
    // ✅ wss destroys ssl → ssl destroys tcp (automatic chain)
    // ✅ No leaks!
}

// ==================== EXAMPLE 5: RECONNECTION SCENARIO ====================

/**
 * BEFORE: Leak on every reconnection
 */
void example_reconnect_before(void)
{
    esp_transport_handle_t ws = create_websocket_transport();
    
    for (int i = 0; i < 100; i++) {
        esp_transport_connect(ws, "example.com", 80, 5000);
        // ... use connection ...
        esp_transport_close(ws);  // ❌ Resources leaked here!
    }
    
    esp_transport_destroy(ws);
    // ❌ 100 reconnections × 424 bytes = 42.4 KB leaked
}

/**
 * AFTER: No leak with resource management
 */
void example_reconnect_after(void)
{
    esp_transport_handle_t ws = create_websocket_transport();
    
    for (int i = 0; i < 100; i++) {
        esp_transport_connect(ws, "example.com", 80, 5000);
        // ... use connection ...
        esp_transport_close(ws);  // ✅ Resources freed immediately
    }
    
    esp_transport_destroy(ws);
    // ✅ 0 bytes leaked!
}

// ==================== EXAMPLE 6: ERROR HANDLING ====================

/**
 * Example with proper error handling
 */
esp_err_t example_error_handling(void)
{
    esp_transport_list_handle_t list = esp_transport_list_init();
    if (!list) {
        return ESP_ERR_NO_MEM;
    }
    
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    if (!tcp) {
        esp_transport_list_destroy(list);
        return ESP_ERR_NO_MEM;
    }
    
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    if (!ws) {
        esp_transport_destroy(tcp);
        esp_transport_list_destroy(list);
        return ESP_ERR_NO_MEM;
    }
    
    // Add to list
    esp_err_t err;
    err = esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    
    err = esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
    if (err != ESP_OK) {
        goto cleanup;
    }
    
    // ... use transports ...
    
    err = ESP_OK;
    
cleanup:
    esp_transport_list_destroy(list);
    // ✅ Proper cleanup even on error
    return err;
}

// ==================== EXAMPLE 7: MIGRATION PATH ====================

/**
 * Backward compatible - old code still works
 */
void example_backward_compatible(void)
{
    // Old API (still works, defaults to EXCLUSIVE ownership)
    esp_transport_list_handle_t list = esp_transport_list_init();
    esp_transport_handle_t t = esp_transport_tcp_init();
    
    esp_transport_list_add(list, t, "tcp");  // ✅ Still works (EXCLUSIVE)
    
    esp_transport_list_destroy(list);  // ✅ Destroys t (EXCLUSIVE ownership)
}

/**
 * Gradual migration - mix old and new APIs
 */
void example_gradual_migration(void)
{
    esp_transport_list_handle_t list = esp_transport_list_init();
    
    esp_transport_handle_t tcp = esp_transport_tcp_init();
    esp_transport_handle_t ws = esp_transport_ws_init(tcp);
    
    // Mix old and new APIs
    esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);  // New
    esp_transport_list_add(list, ws, "ws");  // Old (defaults to EXCLUSIVE)
    
    esp_transport_list_destroy(list);  // ✅ Works correctly
}

