# Web Server Responsiveness Mitigation Plan

## Problem Statement

During inference rounds (2–15 seconds each), the web interface becomes unresponsive. The root cause is a combination of single-threaded HTTP handler execution, absent synchronization between the flow task (Core 0) and web handlers (Core 1), PSRAM contention, and blocking JPEG encoding on the httpd task. See `reportWebstall.md` for the full root cause analysis.

This plan addresses each root cause with specific, implementable changes ordered by impact and complexity.

---

## Change Overview

| # | Change | Impact | Risk | Effort |
|---|--------|--------|------|--------|
| 1 | Add flow-data mutex | Eliminates data races | Low | Medium |
| 2 | Double-buffer inference results | Web reads never block on inference | Low | Medium |
| 3 | Pre-render JPEG images after each round | Eliminates on-demand JPEG encoding | Low | Medium |
| 4 | Move MJPEG stream to a second httpd | Unblocks main web server during streaming | Low | Medium |
| 5 | Fix `RGBImageLock()` with a real mutex | Eliminates image buffer race condition | Low | Low |
| 6 | Increase camera `fb_count` to 2 | Eliminates frame buffer contention | Low | Low |
| 7 | Make `flowisrunning` and `countRounds` atomic | Eliminates torn reads | None | Low |

---

## Change 1: Add a Flow-Data Mutex

### Rationale

The `ClassFlowControll flowctrl` object is accessed from both Core 0 (flow task) and Core 1 (httpd) without any synchronization. Vectors of `HTMLInfo*`, the `aktstatus` string, and `NumberPost*` results are read by web handlers while the flow task writes to them.

### Implementation

#### 1.1 Declare the mutex globally

**File: `MainFlowControl.h`** — add near the existing `extern` declarations:

```cpp
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

extern SemaphoreHandle_t xFlowMutex;
```

**File: `MainFlowControl.cpp`** — add after the existing globals (line ~38):

```cpp
SemaphoreHandle_t xFlowMutex = NULL;
```

#### 1.2 Create the mutex before any task uses it

**File: `MainFlowControl.cpp`** in `InitializeFlowTask()` (line ~1762), before `xTaskCreatePinnedToCore`:

```cpp
void InitializeFlowTask(void)
{
    // Create mutex before task starts
    if (xFlowMutex == NULL) {
        xFlowMutex = xSemaphoreCreateMutex();
        if (xFlowMutex == NULL) {
            LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Failed to create xFlowMutex!");
            return;
        }
    }

    BaseType_t xReturned;
    // ... existing code ...
```

#### 1.3 Lock during the flow pipeline

**File: `MainFlowControl.cpp`** in `doflow()` (line ~127):

```cpp
bool doflow(void)
{
    std::string zw_time = getCurrentTimeString(LOGFILE_TIME_FORMAT);
    ESP_LOGD(TAG, "doflow - start %s", zw_time.c_str());
    flowisrunning = true;

    // Lock during the entire flow round
    if (xSemaphoreTake(xFlowMutex, pdMS_TO_TICKS(30000)) == pdTRUE) {
        flowctrl.doFlow(zw_time);
        xSemaphoreGive(xFlowMutex);
    } else {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "doflow: Failed to acquire xFlowMutex within 30s");
    }

    flowisrunning = false;
    // ... rest unchanged ...
```

#### 1.4 Lock in web handlers that read flow state

Wrap every handler that calls into `flowctrl` with a **short-timeout** mutex take. If the mutex is held (flow is running), return immediately with a "please wait" response instead of blocking.

**File: `MainFlowControl.cpp`** — Example for `handler_json` (line ~463):

```cpp
esp_err_t handler_json(httpd_req_t *req)
{
    if (bTaskAutoFlowCreated)
    {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_type(req, "application/json");

        if (xSemaphoreTake(xFlowMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
            std::string zw = flowctrl.getJSON();
            xSemaphoreGive(xFlowMutex);

            if (zw.length() > 0) {
                httpd_resp_send(req, zw.c_str(), zw.length());
            } else {
                httpd_resp_send(req, NULL, 0);
            }
        } else {
            // Flow is running — return stale-but-fast response
            httpd_resp_set_status(req, "503 Service Unavailable");
            httpd_resp_set_hdr(req, "Retry-After", "2");
            httpd_resp_send(req, "{\"error\":\"Inference in progress, retry shortly\"}", HTTPD_RESP_USE_STRLEN);
        }
    }
    else
    {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Flow not (yet) started: REST API /json not yet available!");
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}
```

Apply the same pattern to these handlers:
- `handler_wasserzaehler` — wrap the `flowctrl.getReadout()`, `GetAllDigit()`, `GetAllAnalog()` calls
- `handler_openmetrics` — wrap `flowctrl.getNumbers()` call
- `handler_statusflow` — wrap `flowctrl.getActStatusWithTime()` call
- `img_tmp_virtual_handler` → `GetJPG()` / `GetRawJPG()` — wrap the calls

### TODO List — Change 1

- [ ] Add `SemaphoreHandle_t xFlowMutex` declaration to `MainFlowControl.h` and `.cpp`
- [ ] Create mutex in `InitializeFlowTask()` before task creation
- [ ] Wrap `flowctrl.doFlow()` in `doflow()` with mutex take/give
- [ ] Add 500ms-timeout mutex take to `handler_json`
- [ ] Add 500ms-timeout mutex take to `handler_wasserzaehler`
- [ ] Add 500ms-timeout mutex take to `handler_openmetrics`
- [ ] Add 500ms-timeout mutex take to `handler_statusflow`
- [ ] Add 500ms-timeout mutex take to `img_tmp_virtual_handler` (for `GetJPG`/`GetRawJPG`)
- [ ] Return HTTP 503 with `Retry-After: 2` when mutex times out
- [ ] Verify no deadlocks by testing all handlers while flow is running

---

## Change 2: Double-Buffer Inference Results

### Rationale

Even with a mutex, the web handler must wait up to 500ms for the flow round to complete before reading results. A better approach is to **snapshot** the results at the end of each round so the web handlers can always read the last-completed snapshot without any lock.

### Design

```
Flow Task (Core 0):          Web Handlers (Core 1):
                              
doFlow() writes to:           Read from:
  flowctrl.GENERAL[n]          resultSnapshot.jsonCache
  flowctrl.NUMBERS              resultSnapshot.readoutValues[]
  flowctrl.aktstatus            resultSnapshot.algRoiJpgData
  ROI images                    resultSnapshot.roiJpgCache[]
         │                              ▲
         │   end of round               │
         └──── snapshot ────────────────┘
              (under mutex)
```

### Implementation

#### 2.1 Define the snapshot structure

**New file: `code/components/jomjol_flowcontroll/FlowResultSnapshot.h`**

```cpp
#pragma once

#include <string>
#include <vector>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "../../include/defines.h"

struct RoiSnapshot {
    std::string filename;
    float val;
    // Pre-encoded JPEG of the ROI image
    uint8_t jpgData[MAX_JPG_SIZE];
    size_t jpgSize;
};

struct FlowResultSnapshot {
    // Protects reads/writes to this structure
    SemaphoreHandle_t mutex;

    // Cached JSON output
    std::string jsonCache;

    // Per-number readout values
    struct NumberResult {
        std::string name;
        std::string value;
        std::string rawValue;
        std::string preValue;
        std::string error;
        std::string rate;
        std::string timestamp;
    };
    std::vector<NumberResult> numberResults;

    // Status
    std::string aktstatus;
    std::string aktstatusWithTime;
    int roundCount;

    // Pre-encoded ROI images
    std::vector<RoiSnapshot> digitRois;
    std::vector<RoiSnapshot> analogRois;

    // Pre-encoded alignment + ROI overlay image
    uint8_t algRoiJpgData[MAX_JPG_SIZE];
    size_t algRoiJpgSize;

    // Pre-encoded raw camera image
    uint8_t rawJpgData[MAX_JPG_SIZE];
    size_t rawJpgSize;

    bool valid;  // Set true after first successful round

    FlowResultSnapshot() : mutex(NULL), algRoiJpgSize(0), rawJpgSize(0),
                           roundCount(0), valid(false) {
        mutex = xSemaphoreCreateMutex();
    }
};

extern FlowResultSnapshot resultSnapshot;
```

#### 2.2 Populate the snapshot at end of each flow round

**File: `MainFlowControl.cpp`** — after `doflow()` returns, with `xFlowMutex` still held:

```cpp
bool doflow(void)
{
    std::string zw_time = getCurrentTimeString(LOGFILE_TIME_FORMAT);
    flowisrunning = true;

    if (xSemaphoreTake(xFlowMutex, pdMS_TO_TICKS(30000)) == pdTRUE) {
        flowctrl.doFlow(zw_time);

        // Snapshot results while we hold the flow mutex
        updateResultSnapshot();

        xSemaphoreGive(xFlowMutex);
    }

    flowisrunning = false;
    return true;
}

void updateResultSnapshot(void)
{
    if (xSemaphoreTake(resultSnapshot.mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "updateResultSnapshot: mutex timeout");
        return;
    }

    // 1. JSON cache
    resultSnapshot.jsonCache = flowctrl.getJSON();

    // 2. Status
    resultSnapshot.aktstatus = *flowctrl.getActStatus();
    resultSnapshot.aktstatusWithTime = *flowctrl.getActStatusWithTime();
    resultSnapshot.roundCount = countRounds;

    // 3. Number results
    resultSnapshot.numberResults.clear();
    const std::vector<NumberPost*> &numbers = flowctrl.getNumbers();
    for (int i = 0; i < numbers.size(); ++i) {
        FlowResultSnapshot::NumberResult nr;
        nr.name = numbers[i]->name;
        nr.value = numbers[i]->ReturnValue;
        nr.rawValue = numbers[i]->ReturnRawValue;
        nr.preValue = numbers[i]->ReturnPreValue;
        nr.error = numbers[i]->ErrorMessageText;
        nr.rate = numbers[i]->ReturnRateValue;
        nr.timestamp = numbers[i]->timeStamp;
        resultSnapshot.numberResults.push_back(nr);
    }

    // 4. Pre-encode digit ROI images as JPEG
    resultSnapshot.digitRois.clear();
    std::vector<HTMLInfo*> digInfo = flowctrl.GetAllDigit();
    for (int i = 0; i < digInfo.size(); ++i) {
        RoiSnapshot rs;
        rs.filename = digInfo[i]->filename;
        rs.val = digInfo[i]->val;
        rs.jpgSize = 0;
        if (digInfo[i]->image) {
            ImageData *id = digInfo[i]->image->writeToMemoryAsJPG();
            if (id && id->size > 0 && id->size <= MAX_JPG_SIZE) {
                memcpy(rs.jpgData, id->data, id->size);
                rs.jpgSize = id->size;
            }
            delete id;
        }
        resultSnapshot.digitRois.push_back(rs);
        delete digInfo[i];
    }

    // 5. Pre-encode analog ROI images as JPEG
    resultSnapshot.analogRois.clear();
    std::vector<HTMLInfo*> anaInfo = flowctrl.GetAllAnalog();
    for (int i = 0; i < anaInfo.size(); ++i) {
        RoiSnapshot rs;
        rs.filename = anaInfo[i]->filename;
        rs.val = anaInfo[i]->val;
        rs.jpgSize = 0;
        if (anaInfo[i]->image) {
            ImageData *id = anaInfo[i]->image->writeToMemoryAsJPG();
            if (id && id->size > 0 && id->size <= MAX_JPG_SIZE) {
                memcpy(rs.jpgData, id->data, id->size);
                rs.jpgSize = id->size;
            }
            delete id;
        }
        resultSnapshot.analogRois.push_back(rs);
        delete anaInfo[i];
    }

    // 6. Pre-encode the alignment ROI overlay image
    resultSnapshot.algRoiJpgSize = 0;
    // (only if the ALGROI_LOAD_FROM_MEM_AS_JPG path is used, skip this — it's already cached)
    // Otherwise, create the overlay and encode it:
    #ifndef ALGROI_LOAD_FROM_MEM_AS_JPG
    if (flowctrl.flowalignment && flowctrl.flowalignment->ImageBasis->ImageOkay()) {
        CImageBasis *overlay = new CImageBasis("snapshot_alg_roi", flowctrl.flowalignment->ImageBasis);
        if (overlay->ImageOkay()) {
            if (flowctrl.flowalignment) flowctrl.flowalignment->DrawRef(overlay);
            if (flowctrl.flowdigit) flowctrl.flowdigit->DrawROI(overlay);
            if (flowctrl.flowanalog) flowctrl.flowanalog->DrawROI(overlay);

            ImageData *id = overlay->writeToMemoryAsJPG();
            if (id && id->size > 0 && id->size <= MAX_JPG_SIZE) {
                memcpy(resultSnapshot.algRoiJpgData, id->data, id->size);
                resultSnapshot.algRoiJpgSize = id->size;
            }
            delete id;
        }
        delete overlay;
    }
    #endif

    resultSnapshot.valid = true;

    xSemaphoreGive(resultSnapshot.mutex);

    LogFile.WriteToFile(ESP_LOG_DEBUG, TAG, "Result snapshot updated");
}
```

#### 2.3 Serve from snapshot in web handlers

**Example: `handler_json`** — replace the `flowctrl.getJSON()` call:

```cpp
esp_err_t handler_json(httpd_req_t *req)
{
    if (!bTaskAutoFlowCreated || !resultSnapshot.valid) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Flow not (yet) started");
        return ESP_ERR_NOT_FOUND;
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_type(req, "application/json");

    if (xSemaphoreTake(resultSnapshot.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        std::string json = resultSnapshot.jsonCache;  // Copy under lock
        xSemaphoreGive(resultSnapshot.mutex);

        httpd_resp_send(req, json.c_str(), json.length());
    } else {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_hdr(req, "Retry-After", "1");
        httpd_resp_send(req, "{\"error\":\"Results being updated\"}", HTTPD_RESP_USE_STRLEN);
    }

    return ESP_OK;
}
```

**Example: serving pre-encoded ROI images from `GetJPGStream()`:**

```cpp
// In ClassFlowControll::GetJPGStream or the img_tmp_virtual_handler,
// check the snapshot first before falling back to live encoding:

if (xSemaphoreTake(resultSnapshot.mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
    // Check digit ROIs
    for (const auto &roi : resultSnapshot.digitRois) {
        if (_fn == roi.filename && roi.jpgSize > 0) {
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_send(req, (const char *)roi.jpgData, roi.jpgSize);
            xSemaphoreGive(resultSnapshot.mutex);
            return ESP_OK;
        }
    }
    // Check analog ROIs
    for (const auto &roi : resultSnapshot.analogRois) {
        if (_fn == roi.filename && roi.jpgSize > 0) {
            httpd_resp_set_type(req, "image/jpeg");
            httpd_resp_send(req, (const char *)roi.jpgData, roi.jpgSize);
            xSemaphoreGive(resultSnapshot.mutex);
            return ESP_OK;
        }
    }
    // Check alg_roi.jpg
    if (_fn == "alg_roi.jpg" && resultSnapshot.algRoiJpgSize > 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_send(req, (const char *)resultSnapshot.algRoiJpgData, resultSnapshot.algRoiJpgSize);
        xSemaphoreGive(resultSnapshot.mutex);
        return ESP_OK;
    }
    xSemaphoreGive(resultSnapshot.mutex);
}
// Fall through to existing live-encode path if snapshot not available
```

### TODO List — Change 2

- [ ] Create `FlowResultSnapshot.h` with snapshot structure
- [ ] Add `FlowResultSnapshot resultSnapshot;` global in `MainFlowControl.cpp`
- [ ] Implement `updateResultSnapshot()` that copies all results + pre-encodes JPEGs
- [ ] Call `updateResultSnapshot()` at end of `doflow()` under flow mutex
- [ ] Rewrite `handler_json` to read from snapshot
- [ ] Rewrite `handler_openmetrics` to read from snapshot
- [ ] Rewrite `handler_wasserzaehler` to read from snapshot
- [ ] Rewrite `img_tmp_virtual_handler` / `GetJPGStream` to serve pre-encoded JPEGs from snapshot
- [ ] Add `flowalignment`/`flowdigit`/`flowanalog` accessors if needed (currently `protected`)
- [ ] Test that snapshot remains valid across rounds
- [ ] Verify PSRAM usage doesn't exceed budget (MAX_JPG_SIZE * (N_digit + N_analog + 1) extra bytes)

---

## Change 3: Move MJPEG Stream to a Second httpd Instance

### Rationale

`CaptureToStream()` runs an infinite `while(1)` loop inside the httpd handler, blocking all other HTTP requests for the entire duration of the stream. A second httpd instance on a different port dedicates a separate task to streaming without affecting the main web UI.

### Implementation

#### 3.1 Create a second httpd server for streaming

**File: `MainFlowControl.cpp`** — new function:

```cpp
static httpd_handle_t stream_httpd = NULL;

esp_err_t start_stream_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.task_priority = tskIDLE_PRIORITY + 2;  // Lower than main httpd
    config.stack_size = 8192;
    config.core_id = 1;          // Same core as main httpd
    config.server_port = 81;     // Different port!
    config.ctrl_port = 32769;
    config.max_open_sockets = 2; // Streams are expensive; limit to 2
    config.max_uri_handlers = 2;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;

    esp_err_t ret = httpd_start(&stream_httpd, &config);
    if (ret != ESP_OK) {
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Failed to start stream httpd on port 81");
        return ret;
    }

    httpd_uri_t stream_uri = {
        .uri       = "/stream",
        .method    = HTTP_GET,
        .handler   = handler_stream,
        .user_ctx  = NULL
    };
    httpd_register_uri_handler(stream_httpd, &stream_uri);

    LogFile.WriteToFile(ESP_LOG_INFO, TAG, "Stream server started on port 81");
    return ESP_OK;
}
```

#### 3.2 Remove the `/stream` handler from the main httpd

**File: `MainFlowControl.cpp`** in `register_server_main_flow_task_uri()` — remove or comment out the existing `/stream` registration (line ~1881):

```cpp
    // REMOVED: Stream now served on port 81
    // camuri.uri = "/stream";
    // camuri.handler = APPLY_BASIC_AUTH_FILTER(handler_stream);
    // camuri.user_ctx = (void *)"stream";
    // httpd_register_uri_handler(server, &camuri);
```

#### 3.3 Start the stream server from `app_main`

**File: `main.cpp`** — after `start_webserver()`:

```cpp
    start_webserver();
    start_stream_server();  // Add this line
```

#### 3.4 Update the web UI to use port 81

**File: relevant HTML/JS in `/sdcard/html/`** — update the stream `<img>` tag source:

```html
<!-- Before: -->
<img src="/stream">

<!-- After: use same host, different port -->
<img id="streamImg">
<script>
    document.getElementById('streamImg').src = 
        window.location.protocol + '//' + window.location.hostname + ':81/stream';
</script>
```

### TODO List — Change 3

- [ ] Add `start_stream_server()` function in `MainFlowControl.cpp`
- [ ] Remove `/stream` URI registration from main httpd
- [ ] Call `start_stream_server()` from `app_main` after `start_webserver()`
- [ ] Update HTML stream source to point to port 81
- [ ] Test that main web UI remains responsive while stream is active
- [ ] Test that stream works correctly on port 81
- [ ] Consider authentication for port 81 if `BASIC_AUTH` is enabled

---

## Change 4: Fix `RGBImageLock()` with a Real FreeRTOS Mutex

### Rationale

The current `RGBImageLock()` in `CImageBasis.cpp` is a non-functional lock: it checks a `bool islocked` flag but never sets it to `true`. Two concurrent callers both pass through. The polling wait uses 1-second `vTaskDelay()` granularity.

### Implementation

#### 4.1 Replace `bool islocked` with a `SemaphoreHandle_t` in `CImageBasis`

**File: `CImageBasis.h`** — change the member:

```cpp
class CImageBasis
{
    protected:
        bool externalImage;
        // ...
        SemaphoreHandle_t imageMutex;  // Replaces: bool islocked;

    public:
        // ...
        uint8_t * RGBImageLock(int _waitmaxsec = 60);
        void RGBImageRelease();
```

#### 4.2 Initialize the mutex in constructors

**File: `CImageBasis.cpp`** — in every constructor:

```cpp
CImageBasis::CImageBasis(/* ... */)
{
    imageMutex = xSemaphoreCreateMutex();
    // ... rest of constructor ...
}
```

And free it in the destructor:

```cpp
CImageBasis::~CImageBasis()
{
    if (imageMutex) {
        vSemaphoreDelete(imageMutex);
        imageMutex = NULL;
    }
    // ... existing cleanup ...
}
```

#### 4.3 Rewrite `RGBImageLock()` and `RGBImageRelease()`

**File: `CImageBasis.cpp`**:

```cpp
uint8_t * CImageBasis::RGBImageLock(int _waitmaxsec)
{
    if (!imageMutex) return NULL;

    if (xSemaphoreTake(imageMutex, pdMS_TO_TICKS(_waitmaxsec * 1000)) == pdTRUE) {
        return rgb_image;
    }

    ESP_LOGW(TAG, "RGBImageLock: timeout after %ds for image '%s'", _waitmaxsec, name.c_str());
    return NULL;
}

void CImageBasis::RGBImageRelease()
{
    if (imageMutex) {
        xSemaphoreGive(imageMutex);
    }
}
```

### TODO List — Change 4

- [ ] Replace `bool islocked` with `SemaphoreHandle_t imageMutex` in `CImageBasis.h`
- [ ] Initialize `imageMutex` in all `CImageBasis` constructors
- [ ] Delete `imageMutex` in `CImageBasis` destructor
- [ ] Rewrite `RGBImageLock()` to use `xSemaphoreTake()`
- [ ] Rewrite `RGBImageRelease()` to use `xSemaphoreGive()`
- [ ] Audit all call sites to ensure every `RGBImageLock()` has a matching `RGBImageRelease()`
- [ ] Ensure no code path returns between Lock and Release without releasing (use RAII wrapper if needed)

---

## Change 5: Increase Camera Frame Buffer Count

### Rationale

With `fb_count = 1`, the camera driver has only one frame buffer. When the flow task holds it during JPEG→RGB decode, a web request for `raw.jpg` blocks on `esp_camera_fb_get()`. With `fb_count = 2`, a second buffer is available for concurrent access.

### Implementation

**File: `ClassControllCamera.cpp`** line 102:

```cpp
// Before:
    .fb_count = 1,

// After:
    .fb_count = 2,
```

### Memory Cost

One VGA JPEG frame buffer ≈ 50–100 KB in PSRAM. With 4 MB PSRAM total, 2 MB already allocated for model + tensor arena, and ~900 KB for images, this leaves ~1.1 MB free — an extra 100 KB is feasible.

### TODO List — Change 5

- [ ] Change `.fb_count = 1` to `.fb_count = 2` in `ClassControllCamera.cpp:102`
- [ ] Verify PSRAM free heap after boot is still > 400 KB
- [ ] Test `raw.jpg` requests during inference round
- [ ] Test that flow image capture still works correctly

---

## Change 6: Make Shared Flags Atomic

### Rationale

`flowisrunning` (bool) and `countRounds` (int) are read from Core 1 (web handlers) and written from Core 0 (flow task) without synchronization. On the ESP32's dual-core Xtensa architecture, non-atomic reads can see torn values for multi-byte types.

### Implementation

**File: `MainFlowControl.cpp`** — change declarations:

```cpp
#include <atomic>

// Before:
// bool flowisrunning = false;
// int countRounds = 0;

// After:
std::atomic<bool> flowisrunning{false};
std::atomic<int> countRounds{0};
```

**File: `MainFlowControl.h`** — if these are `extern`-declared anywhere, update:

```cpp
#include <atomic>
extern std::atomic<bool> flowisrunning;
extern std::atomic<int> countRounds;
```

All existing reads (`if (flowisrunning)`, `countRounds++`) remain syntactically valid because `std::atomic` has implicit conversion operators.

### TODO List — Change 6

- [ ] Change `bool flowisrunning` to `std::atomic<bool> flowisrunning{false}`
- [ ] Change `int countRounds` to `std::atomic<int> countRounds{0}`
- [ ] Update any `extern` declarations in headers
- [ ] Compile and verify no type errors

---

## Change 7: Protect PSRAM Shared Region State

### Rationale

`sharedMemoryInUseFor` in `psram.cpp` is a `std::string` modified without locking. While it is normally only written sequentially by the flow pipeline on Core 0, adding a lightweight spinlock prevents future regressions and documents the intended exclusion.

### Implementation

**File: `psram.cpp`** — add a spinlock:

```cpp
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static portMUX_TYPE psramMux = portMUX_INITIALIZER_UNLOCKED;

bool psram_init_shared_memory_for_take_image_step(void) {
    portENTER_CRITICAL(&psramMux);
    if (sharedMemoryInUseFor != "") {
        portEXIT_CRITICAL(&psramMux);
        LogFile.WriteToFile(ESP_LOG_ERROR, TAG, "Shared memory already in use for " + sharedMemoryInUseFor);
        return false;
    }
    allocatedBytesForSTBI = 0;
    sharedMemoryInUseFor = "TakeImage";
    portEXIT_CRITICAL(&psramMux);
    return true;
}

void psram_deinit_shared_memory_for_take_image_step(void) {
    portENTER_CRITICAL(&psramMux);
    allocatedBytesForSTBI = 0;
    sharedMemoryInUseFor = "";
    portEXIT_CRITICAL(&psramMux);
}
```

Apply the same pattern to:
- `psram_reserve_shared_tmp_image_memory()` / `psram_free_shared_temp_image_memory()`
- `psram_get_shared_tensor_arena_memory()` / `psram_get_shared_model_memory()` / `psram_free_shared_tensor_arena_and_model_memory()`

### TODO List — Change 7

- [ ] Add `portMUX_TYPE psramMux` in `psram.cpp`
- [ ] Wrap all `sharedMemoryInUseFor` reads/writes with `portENTER_CRITICAL`/`portEXIT_CRITICAL`
- [ ] Verify no ISR context calls these functions (critical sections are not ISR-safe)
- [ ] Test that flow pipeline timing is not affected (critical sections are <1μs)

---

## Implementation Order

Execute in this order to minimize risk and get early wins:

```
Phase 1 — Quick Wins (Low risk, immediate improvement)
  ├── Change 5: fb_count = 2
  ├── Change 6: Atomic flags
  └── Change 4: RGBImageLock mutex

Phase 2 — Core Fix (Medium effort, major improvement)
  ├── Change 1: Flow data mutex
  └── Change 7: PSRAM spinlock

Phase 3 — Full Solution (Higher effort, eliminates stalls)
  ├── Change 2: Double-buffer + pre-encode JPEGs
  └── Change 3: Separate stream httpd
```

---

## Test Strategy

### Test Environment Setup

1. **Hardware**: ESP32-CAM board (AI Thinker) with SD card and configured meter
2. **Network**: WiFi connection with a client PC for HTTP load testing
3. **Tools**:
   - `curl` for individual API endpoint testing
   - `ab` (Apache Bench) or `hey` for concurrent load testing
   - Serial monitor for ESP_LOG output and crash analysis
   - Browser Developer Tools (Network tab) for page load waterfall analysis

### Test 1: Baseline Measurement (Before Changes)

Capture quantitative baselines to compare against:

```bash
# Measure /json response time during idle (no inference)
for i in $(seq 1 20); do
  curl -o /dev/null -s -w "%{time_total}\n" http://<device>/json
done > baseline_idle.txt

# Measure /json response time during inference
# (trigger flow start, then immediately send requests)
curl -s http://<device>/flow_start
for i in $(seq 1 20); do
  curl -o /dev/null -s -w "%{time_total}\n" http://<device>/json
done > baseline_inference.txt

# Measure full page load time
curl -o /dev/null -s -w "%{time_total}\n" "http://<device>/value?full"

# Measure concurrent request handling
ab -n 50 -c 5 http://<device>/json
```

Record:
- **P50, P95, P99 response times** for `/json`, `/value`, `/img_tmp/alg_roi.jpg`
- **Error rate** (HTTP 5xx, connection refused, timeout)
- **Page load time** for `/value?full` in browser
- **Free heap** during idle and during inference

### Test 2: Mutex Correctness (After Change 1)

```bash
# Verify no deadlocks — repeatedly hit endpoints during inference
while true; do
  curl -s -m 3 http://<device>/json > /dev/null &
  curl -s -m 3 http://<device>/value > /dev/null &
  curl -s -m 3 http://<device>/statusflow > /dev/null &
  curl -s -m 3 http://<device>/metrics > /dev/null &
  wait
done

# Run for 10 minutes. Monitor serial output for:
#   - "Failed to acquire xFlowMutex" (expected occasionally)
#   - Stack overflow / guru meditation (should not occur)
#   - Watchdog timeout (should not occur)
```

**Pass criteria**:
- No crashes or watchdog timeouts
- HTTP 503 returned when mutex unavailable (not a hang)
- All requests complete within 3 seconds (curl -m 3)

### Test 3: Snapshot Correctness (After Change 2)

```bash
# Verify JSON content matches expectations
curl -s http://<device>/json | python3 -m json.tool

# Verify snapshot updates after each round
# Run two rounds and confirm values change:
curl -s http://<device>/flow_start
sleep 30  # Wait for round to complete
ROUND1=$(curl -s http://<device>/json)
curl -s http://<device>/flow_start
sleep 30
ROUND2=$(curl -s http://<device>/json)
# ROUND1 and ROUND2 should have different timestamps

# Verify pre-encoded ROI images are valid JPEGs
curl -s http://<device>/img_tmp/dig1_roi0.jpg -o roi.jpg
file roi.jpg  # Should say: JPEG image data
identify roi.jpg  # Should show valid dimensions
```

**Pass criteria**:
- JSON always parseable (no truncation or corruption)
- ROI images are valid JPEGs with expected dimensions
- Values update correctly after each round
- `alg_roi.jpg` shows correct ROI overlay

### Test 4: Stream Isolation (After Change 3)

```bash
# Start stream on port 81
# Open in browser: http://<device>:81/stream

# While stream is active, verify main httpd is responsive
for i in $(seq 1 50); do
  curl -o /dev/null -s -w "%{time_total}\n" -m 3 http://<device>/json
done > stream_test.txt

# All response times should be < 1s
awk '{if ($1 > 1.0) print "FAIL: " $1; else print "PASS: " $1}' stream_test.txt
```

**Pass criteria**:
- Main httpd responds to all requests within 1s while stream is active
- Stream displays correctly on port 81
- No "connection refused" on port 80

### Test 5: Concurrent Load During Inference (After All Changes)

```bash
# Trigger flow, then hammer the device
curl -s http://<device>/flow_start

# Run 100 requests across 5 concurrent connections
ab -n 100 -c 5 -s 5 http://<device>/json

# Expected results:
#   - 0% failed requests
#   - Mean response time < 500ms
#   - P99 response time < 2s
#   - Some HTTP 503 responses are acceptable (= inference in progress)
```

### Test 6: Memory Stability (Endurance Test)

```bash
# Run overnight: trigger flow + hit all endpoints every 30 seconds
while true; do
  curl -s http://<device>/flow_start
  sleep 5
  curl -s http://<device>/json > /dev/null
  curl -s http://<device>/value > /dev/null
  curl -s http://<device>/img_tmp/alg_roi.jpg > /dev/null
  curl -s http://<device>/metrics > /dev/null
  curl -s http://<device>/sysinfo > /dev/null
  # Check heap
  HEAP=$(curl -s http://<device>/sysinfo | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['freeHeapMem'])")
  echo "$(date): Free heap = $HEAP"
  sleep 25
done > endurance.log 2>&1
```

**Pass criteria**:
- Free heap does not monotonically decrease (no memory leak)
- No crashes over 12+ hours
- All endpoints remain responsive

### Test 7: Race Condition Stress Test (After Change 4)

```bash
# Rapidly request the same image to exercise the RGBImageLock mutex
for i in $(seq 1 100); do
  curl -s -m 3 http://<device>/img_tmp/alg_roi.jpg -o /dev/null &
done
wait

# Monitor for:
#   - "RGBImageLock: timeout" in serial log (acceptable)
#   - Guru meditation error (should NOT occur)
#   - Corrupted JPEG (examine saved files; should NOT occur)
```

**Pass criteria**:
- No crashes with 100 concurrent image requests
- All returned JPEGs are valid (no corrupt headers)
- Timeout logged but handled gracefully

### Summary of Pass/Fail Criteria

| # | Test | Key Metric | Pass Threshold |
|---|------|-----------|----------------|
| 1 | Baseline | Document current perf | N/A (reference) |
| 2 | Mutex | No crashes under load | 0 crashes in 10 min |
| 3 | Snapshot | Correct data served | 100% valid JSON + JPEG |
| 4 | Stream isolation | Main httpd latency | P99 < 1s during stream |
| 5 | Concurrent load | Request success rate | > 95% success, P99 < 2s |
| 6 | Memory stability | Heap stability | No leak over 12 hours |
| 7 | Race stress | Image corruption | 0 corrupt images, 0 crashes |

---

## Risk Assessment

| Change | What Could Go Wrong | Mitigation |
|--------|---------------------|------------|
| 1 (Mutex) | Deadlock if handler takes mutex then calls code that also takes it | Use single non-recursive mutex; audit all call chains |
| 1 (Mutex) | Priority inversion: httpd (prio 3) holds mutex, flow task (prio 2) waits | FreeRTOS mutexes have built-in priority inheritance |
| 2 (Snapshot) | PSRAM exhaustion from pre-encoded JPEGs | Budget: ~12 ROIs × 128KB = ~1.5 MB max. Check heap during test |
| 2 (Snapshot) | Stale data if snapshot update fails | Log errors; web handler falls back to "no data" response |
| 3 (Stream httpd) | Two httpd tasks fighting for camera | Already handled by `fb_count = 2` (Change 5) |
| 4 (Image mutex) | Existing code forgets to release mutex | Audit all `RGBImageLock()` call sites; consider RAII scope guard |
| 5 (fb_count) | More PSRAM used for second frame buffer | ~100 KB; verify sufficient headroom |
| 6 (Atomics) | C++ `<atomic>` support on Xtensa | ESP-IDF 5.x supports `std::atomic`; verified in ESP32 toolchain |
| 7 (PSRAM spinlock) | `portENTER_CRITICAL` disables interrupts | Sections are < 1μs (string compare + assign); negligible impact |

---

## Files to Modify

| File | Changes |
|------|---------|
| `MainFlowControl.h` | Add `xFlowMutex` extern, atomic includes |
| `MainFlowControl.cpp` | Mutex init, `doflow()` locking, handler rewrites, snapshot, stream server |
| `FlowResultSnapshot.h` | **NEW** — snapshot structure definition |
| `CImageBasis.h` | Replace `bool islocked` with `SemaphoreHandle_t imageMutex` |
| `CImageBasis.cpp` | Rewrite `RGBImageLock()`/`RGBImageRelease()`, constructor/destructor |
| `ClassControllCamera.cpp` | Change `fb_count` to 2 |
| `ClassFlowControll.h` | Make `flowalignment`/`flowdigit`/`flowanalog` accessible for snapshot |
| `ClassFlowControll.cpp` | Optionally add snapshot-aware GetJPGStream |
| `psram.cpp` | Add `portMUX_TYPE` spinlock around `sharedMemoryInUseFor` |
| `main.cpp` | Call `start_stream_server()` |
| `sdcard/html/*.html` | Update stream URL to port 81 |
