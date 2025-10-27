# Phase 1 Quick Start Guide
## ESP-IDF Transport Layer Refactoring

**TL;DR**: Fix 3 critical bugs (ownership, cleanup, parent chain) with backward-compatible APIs

---

## 📋 What You Get

| Before Phase 1 | After Phase 1 |
|----------------|---------------|
| ❌ 424 bytes leaked/reconnection | ✅ 0 bytes leaked |
| ❌ Ambiguous ownership → double-frees | ✅ Explicit ownership → no double-frees |
| ❌ Parent transport leaked | ✅ Automatic parent cleanup |
| ❌ Manual resource management | ✅ Automatic resource tracking |

---

## 🚀 Quick Implementation

### Step 1: Add New Files (5 minutes)

```bash
cd $IDF_PATH/components/tcp_transport

# 1. Copy new files
cp docs/transport_resources.c ./
cp docs/esp_transport_resources.h include/

# 2. Update CMakeLists.txt
# Add "transport_resources.c" to SRCS list
```

### Step 2: Update Headers (10 minutes)

**esp_transport.h** - Add these 3 declarations:
```c
// 1. Ownership enum
typedef enum {
    ESP_TRANSPORT_OWNERSHIP_NONE = 0,
    ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE = 2
} esp_transport_ownership_t;

// 2. Extended add function
esp_err_t esp_transport_list_add_ex(
    esp_transport_list_handle_t list,
    esp_transport_handle_t t,
    const char *scheme,
    esp_transport_ownership_t ownership);

// 3. Chain destroy function
esp_err_t esp_transport_destroy_chain(esp_transport_handle_t t);
```

**esp_transport_internal.h** - Add these 2 fields to `struct esp_transport_item_t`:
```c
struct esp_transport_item_t {
    // ... existing fields ...
    
    esp_transport_ownership_t ownership;  // NEW
    esp_transport_handle_t parent;        // NEW
    
    STAILQ_ENTRY(esp_transport_item_t) next;
};
```

### Step 3: Update transport.c (15 minutes)

Add these 4 functions (copy from `docs/PHASE1_TRANSPORT_C_ADDITIONS.c`):

1. `esp_transport_list_add_ex()` - New ownership-aware add
2. Updated `esp_transport_list_clean()` - Respect ownership
3. `esp_transport_destroy_chain()` - Automatic parent cleanup
4. Updated `esp_transport_init()` - Initialize new fields

### Step 4: Update WebSocket Transport (20 minutes)

**transport_ws.c** - Apply 5 changes:

```c
// 1. Add resource tracking to structure
typedef struct transport_ws {
    // ... existing fields ...
    transport_resource_t resources[8];  // NEW
} transport_ws_t;

// 2. In init: Set parent and define resources
t->parent = parent_handle;  // NEW
ws->resources[0] = TRANSPORT_RESOURCE(ws->buffer, init_fn, cleanup_fn);  // NEW
// ... define other resources ...

// 3. In connect: Initialize resources
transport_resources_init(ws->resources, NULL);  // NEW (replaces malloc)

// 4. In close: Cleanup resources
transport_resources_cleanup(ws->resources);  // NEW (replaces manual free)

// 5. In destroy: Idempotent cleanup (parent automatic)
transport_resources_cleanup(ws->resources);  // NEW
// Remove: esp_transport_destroy(ws->parent);  // Now automatic
```

### Step 5: Update Application Code (10 minutes)

**esp_websocket_client.c** - Change list add calls:

```c
// Before:
esp_transport_list_add(list, tcp, "_tcp");
esp_transport_list_add(list, ws, "ws");

// After:
esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
```

### Step 6: Test (30 minutes)

```bash
# Build with AddressSanitizer
idf.py -DCONFIG_COMPILER_ASAN=y build

# Run memory leak test
./test_websocket_reconnect 100

# Expected: 0 bytes leaked (was 42.4 KB)
```

---

## 📖 Core Concepts

### Concept 1: Explicit Ownership

**Problem**: Who destroys the parent transport?

```c
esp_transport_handle_t tcp = esp_transport_tcp_init();
esp_transport_handle_t ws = esp_transport_ws_init(tcp);

esp_transport_list_add(list, tcp, "_tcp");  // ❌ Ambiguous
```

**Solution**: Be explicit

```c
// tcp is owned by ws (parent chain), not by list
esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);

// ws is owned by list
esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
```

### Concept 2: Resource Management

**Problem**: Resources leaked on `close()`

```c
connect() { ws->buffer = malloc(2048); }  // Allocate
close()   { /* nothing */ }               // ❌ Leak!
destroy() { free(ws->buffer); }           // Too late
```

**Solution**: Track and cleanup

```c
init()    { ws->resources[0] = TRANSPORT_RESOURCE(ws->buffer, init, cleanup); }
connect() { transport_resources_init(ws->resources, NULL); }  // Allocate
close()   { transport_resources_cleanup(ws->resources); }     // ✅ Free!
destroy() { transport_resources_cleanup(ws->resources); }     // Idempotent
```

### Concept 3: Chain Operations

**Problem**: Forgot to destroy parent

```c
ws_destroy(ws) {
    free(ws->buffer);
    // ❌ Forgot: esp_transport_destroy(ws->parent);
}
```

**Solution**: Automatic propagation

```c
// Set parent in init:
t->parent = parent_handle;

// Destroy is automatic:
esp_transport_destroy_chain(ws);
// → Destroys ws, then automatically destroys parent (tcp)
```

---

## 🎯 Decision Tree

### When to use `OWNERSHIP_NONE` vs `OWNERSHIP_EXCLUSIVE`?

```
Is this transport used as a parent by another transport in the list?
│
├─ YES → Use OWNERSHIP_NONE
│   Example: tcp is parent of ws
│   esp_transport_list_add_ex(list, tcp, "_tcp", OWNERSHIP_NONE);
│
└─ NO → Use OWNERSHIP_EXCLUSIVE
    Example: ws is top-level transport
    esp_transport_list_add_ex(list, ws, "ws", OWNERSHIP_EXCLUSIVE);
```

### When to use `esp_transport_destroy()` vs `esp_transport_destroy_chain()`?

```
Does this transport have a parent?
│
├─ YES → Use esp_transport_destroy_chain()
│   Example: WebSocket has SSL parent
│   esp_transport_destroy_chain(ws);  // Destroys ws → ssl → tcp
│
└─ NO → Use esp_transport_destroy()
    Example: Standalone TCP transport
    esp_transport_destroy(tcp);  // Just destroys tcp
```

---

## 🔍 Common Patterns

### Pattern 1: WebSocket over TCP

```c
esp_transport_handle_t tcp = esp_transport_tcp_init();
esp_transport_handle_t ws = esp_transport_ws_init(tcp);

esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);

// List owns only ws, ws owns tcp via parent chain
```

### Pattern 2: WebSocket over SSL

```c
esp_transport_handle_t tcp = esp_transport_tcp_init();
esp_transport_handle_t ssl = esp_transport_ssl_init();
esp_transport_handle_t wss = esp_transport_ws_init(ssl);

esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
esp_transport_list_add_ex(list, ssl, "_ssl", ESP_TRANSPORT_OWNERSHIP_NONE);
esp_transport_list_add_ex(list, wss, "wss", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);

// List owns only wss, wss owns ssl, ssl owns tcp (chain)
```

### Pattern 3: Multiple Protocols

```c
// TCP transport (standalone)
esp_transport_handle_t tcp = esp_transport_tcp_init();
esp_transport_list_add_ex(list, tcp, "tcp", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);

// SSL transport (standalone)
esp_transport_handle_t ssl = esp_transport_ssl_init();
esp_transport_list_add_ex(list, ssl, "ssl", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);

// Both owned by list (no parent relationship)
```

---

## ⚠️ Migration Pitfalls

### Pitfall 1: Forgetting to set `t->parent`

```c
// ❌ BAD: Parent not set
esp_transport_handle_t ws = esp_transport_ws_init(tcp);
// Missing: t->parent = tcp;

// Result: esp_transport_destroy_chain(ws) won't destroy tcp
```

**Fix:**
```c
// ✅ GOOD: Set parent in init
esp_transport_handle_t ws = esp_transport_ws_init(tcp);
t->parent = tcp;  // Add this line
```

### Pitfall 2: Using wrong ownership

```c
// ❌ BAD: Both have EXCLUSIVE ownership
esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);

// Result: List tries to destroy tcp, ws also tries to destroy tcp (double-free!)
```

**Fix:**
```c
// ✅ GOOD: tcp has NONE (ws owns it)
esp_transport_list_add_ex(list, tcp, "_tcp", ESP_TRANSPORT_OWNERSHIP_NONE);
esp_transport_list_add_ex(list, ws, "ws", ESP_TRANSPORT_OWNERSHIP_EXCLUSIVE);
```

### Pitfall 3: Not defining sentinel in resources array

```c
// ❌ BAD: No sentinel
transport_resource_t resources[2];
resources[0] = TRANSPORT_RESOURCE(buffer, init, cleanup);
resources[1] = TRANSPORT_RESOURCE(host, NULL, cleanup);
// Missing sentinel!

// Result: Loops past array bounds
```

**Fix:**
```c
// ✅ GOOD: Explicit sentinel
transport_resource_t resources[3];  // +1 for sentinel
resources[0] = TRANSPORT_RESOURCE(buffer, init, cleanup);
resources[1] = TRANSPORT_RESOURCE(host, NULL, cleanup);
resources[2] = (transport_resource_t){0};  // Sentinel
```

---

## 📊 Validation Checklist

After implementation, verify:

- [ ] **Memory leaks**: Run Valgrind, expect 0 leaks
- [ ] **Reconnection test**: 100 reconnects, heap stable
- [ ] **Ownership**: List destroy doesn't crash
- [ ] **Parent cleanup**: Parent transport freed
- [ ] **Backward compat**: Old code still works

**Commands:**
```bash
# Memory leak test
valgrind --leak-check=full --show-leak-kinds=all ./build/test_transport

# Reconnection test
./test_websocket_reconnect 100

# Check heap
heap_caps_get_free_size(MALLOC_CAP_DEFAULT)  # Should be stable
```

---

## 📁 File Reference

| File | Purpose |
|------|---------|
| `PHASE1_IMPLEMENTATION.md` | Full specification (50+ pages) |
| `PHASE1_QUICK_START.md` | This file (quick reference) |
| `PHASE1_EXAMPLE_USAGE.c` | Before/after examples |
| `PHASE1_WEBSOCKET_INTEGRATION.c` | WebSocket-specific integration |
| `PHASE1_TRANSPORT_API_ADDITIONS.h` | API declarations to copy |
| `PHASE1_TRANSPORT_C_ADDITIONS.c` | Implementation to copy |
| `transport_resources.c` | NEW FILE (resource management) |
| `esp_transport_resources.h` | NEW FILE (resource API) |

---

## 🎓 Learning Resources

**Read in order:**
1. This file (PHASE1_QUICK_START.md) ← Start here
2. PHASE1_EXAMPLE_USAGE.c (examples)
3. PHASE1_WEBSOCKET_INTEGRATION.c (real-world)
4. PHASE1_IMPLEMENTATION.md (deep dive)

**Quick lookup:**
- "How do I use ownership?" → PHASE1_EXAMPLE_USAGE.c, Example 1
- "How do I track resources?" → PHASE1_EXAMPLE_USAGE.c, Example 2
- "How do I apply to WebSocket?" → PHASE1_WEBSOCKET_INTEGRATION.c
- "What are the APIs?" → PHASE1_TRANSPORT_API_ADDITIONS.h

---

## ⏱️ Estimated Time

| Task | Time | Cumulative |
|------|------|------------|
| Add new files | 5 min | 5 min |
| Update headers | 10 min | 15 min |
| Update transport.c | 15 min | 30 min |
| Update transport_ws.c | 20 min | 50 min |
| Update application code | 10 min | 60 min |
| Testing | 30 min | 90 min |
| **Total** | **1.5 hours** | |

---

## 🆘 Troubleshooting

### Issue: Compile error "ownership undeclared"
**Solution**: Add `esp_transport_ownership_t ownership;` to `struct esp_transport_item_t`

### Issue: Segfault in `esp_transport_list_clean()`
**Solution**: Initialize `t->ownership` in `esp_transport_init()`

### Issue: Parent transport not destroyed
**Solution**: Set `t->parent = parent_handle;` in transport init function

### Issue: Resources not cleaned up
**Solution**: Add sentinel `{0}` to resources array

### Issue: Still have memory leaks
**Solution**: 
1. Check `transport_resources_cleanup()` is called in `close()`
2. Verify resources array is properly defined
3. Run with Valgrind to find exact leak location

---

## ✅ Success Criteria

You've successfully completed Phase 1 when:

1. ✅ Valgrind reports 0 bytes leaked
2. ✅ 100 reconnections show stable heap usage
3. ✅ All unit tests pass
4. ✅ No AddressSanitizer warnings
5. ✅ Old code still works (backward compatible)

**Expected improvement:**
```
Before: 424 bytes leaked per reconnection
After:  0 bytes leaked per reconnection
Savings: 424 bytes × 1000 reconnections = 424 KB saved
```

---

## 🚦 Next Steps

After Phase 1 is complete and validated:
- **Phase 2**: Apply to all transports + simple state tracking (3 weeks)
- **Phase 3**: Memory pooling optimization (2 weeks)
- **Phase 4-5**: Complete migration + polish

---

**Questions?** Check `PHASE1_IMPLEMENTATION.md` for detailed explanations

**Ready to start?** Follow Step 1 above ☝️

