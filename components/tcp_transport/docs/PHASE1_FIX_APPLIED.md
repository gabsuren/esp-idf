# Phase 1 Fix Applied - Resource Cleanup on Reconnect

**Issue Found:** Memory leak still occurring (~400 bytes/cycle)

**Root Cause:** The WebSocket client may be calling `destroy()` without calling `close()` first, leaving session resources allocated.

---

## 🔧 Additional Fix Applied

### **Fix 1: Cleanup at Connect**
Added resource cleanup at the START of `ws_connect()` to handle leftover resources from previous connections:

```c
static int ws_connect(...)
{
    // NEW: Cleanup any leftover resources before allocating new ones
    transport_resources_cleanup(ws->resources);
    ESP_LOGI(TAG, "WebSocket connect: cleaned up old resources");
    
    // ... then allocate fresh resources ...
    transport_resources_init(ws->resources, NULL);
    ESP_LOGI(TAG, "WebSocket connect: session resources initialized");
}
```

**Why:** This ensures resources are freed even if `close()` wasn't called between connections.

---

### **Fix 2: INFO-Level Logging**
Changed all resource management logs from `ESP_LOGD` to `ESP_LOGI` so we can see what's happening even without DEBUG level enabled.

**Now you'll see:**
```
I (xxx) transport_ws: WebSocket connect: cleaned up old resources
I (xxx) transport_resources: Cleaning up resource 'ws->buffer'
I (xxx) transport_resources: Cleaned up 1 resources
I (xxx) transport_ws: WebSocket connect: session resources initialized
```

---

## 🔬 Test Again

```bash
cd $IDF_PATH
idf.py build flash monitor
```

**What to Look For:**

### ✅ **Expected: Logs showing cleanup**
```
I transport_ws: WebSocket connect: cleaned up old resources
I transport_resources: Cleaning up resource 'ws->buffer'
I transport_resources: Cleaned up 2 resources
I transport_ws: WebSocket connect: session resources initialized
```

### ✅ **Expected: Memory stable**
```
Cycle 1: Initial 125832 → 125444 (388 bytes - initial fragmentation)
Cycle 2: Initial 125832 → 125444 (0 bytes leaked!) ← STABLE
Cycle 3: Initial 125832 → 125444 (0 bytes leaked!) ← STABLE
Cycle 4: Initial 125832 → 125444 (0 bytes leaked!) ← STABLE
```

The heap might vary by a few hundred bytes due to fragmentation, but should stabilize after the first cycle.

---

## 📊 Why This Fixes The Leak

### Before This Fix:
```
connect() → allocate buffer (2048 bytes)
use connection
[close() NOT CALLED by client]
destroy() → cleanup called, but...
connect() → allocate NEW buffer
           OLD buffer still there → LEAK!
```

### After This Fix:
```
connect() → cleanup old (if any)  ← NEW!
         → allocate buffer (2048 bytes)
use connection
[close() NOT CALLED by client]
destroy() → cleanup called
connect() → cleanup old (frees the 2048 bytes) ← FIXES LEAK!
         → allocate NEW buffer
```

---

## 🐛 Debugging

If still leaking, check:

1. **Are cleanup logs appearing?**
   - NO → Check if sentinel is correct in resources array
   - NO → Check if resources are marked as `initialized = true`

2. **How many resources cleaned?**
   - "Cleaned up 0 resources" → Resources not marked as initialized
   - "Cleaned up 1 resources" → Only buffer cleaned (correct)
   - "Cleaned up 2+ resources" → Buffer + others (also correct)

3. **Is leak still ~400 bytes?**
   - YES → Buffer (2048 bytes) not being freed, cleanup not working
   - NO, smaller → Other leak, not the buffer

---

## 📞 Next Steps

1. **Rebuild:** `idf.py build`
2. **Flash:** `idf.py flash monitor`
3. **Watch logs:** Look for "Cleaning up resource" messages
4. **Check heap:** Should stabilize after first cycle

---

**Status:** ✅ Fix Applied  
**Build Required:** Yes  
**Expected Result:** 0-100 bytes variance (fragmentation only), no leak

