# Root Cause Analysis: Web Interface Unresponsive During Inference

## Executive Summary

The ESP-IDF HTTP server (`httpd`) uses a **single-threaded select()-based event loop** running as one FreeRTOS task on **Core 1** at priority 3. All URI handlers execute **synchronously within this task** — while any handler is running, **no other HTTP request can be processed**. During an inference round, several mechanisms cause handlers to block for hundreds of milliseconds to tens of seconds, making the web interface unresponsive.

---

## 1. System Architecture

### Task Layout

| Task | Core | Priority | Purpose |
|------|------|----------|---------|
| `httpd` (web server) | 1 | 3 | All HTTP request handling |
| `task_autodoFlow` | 0 | 2 | Image capture + CNN inference pipeline |
| `cam_task` | 1 | 27 | Camera DMA frame buffer filling |

### Critical Design Constraint

The httpd server is configured in `server_main.cpp:346-370`:

```
max_open_sockets = 5     (max concurrent TCP connections)
backlog_conn = 5          (pending connection queue)
stack_size = 12288        (12 KB per handler invocation)
core_id = 1               (pinned to Core 1)
task_priority = 3
recv_wait_timeout = 5s
send_wait_timeout = 5s
```

**ESP-IDF's httpd is NOT multi-threaded.** It uses `select()` to multiplex sockets, but handler functions execute **sequentially** in the httpd task. While one handler runs, all other connections queue behind it.

---

## 2. The Blocking Chain: What Happens During Inference

### 2.1 The Flow Pipeline (Core 0)

`task_autodoFlow` runs a processing cycle every `auto_interval` ms (typically 60–300 seconds). Each cycle executes these steps sequentially in `ClassFlowControll::doFlow()` (`ClassFlowControll.cpp:391`):

```
FlowControll[0]->doFlow()  → ClassFlowTakeImage::doFlow()     [Camera capture]
FlowControll[1]->doFlow()  → ClassFlowAlignment::doFlow()     [Template matching]
FlowControll[2]->doFlow()  → ClassFlowCNNGeneral::doFlow()    [Digit CNN inference]
FlowControll[3]->doFlow()  → ClassFlowCNNGeneral::doFlow()    [Analog CNN inference]
FlowControll[4]->doFlow()  → ClassFlowPostProcessing::doFlow() [Result validation]
FlowControll[5]->doFlow()  → ClassFlowMQTT/InfluxDB/Webhook   [Network publish]
```

**Total pipeline duration: 2–15 seconds** depending on number of ROIs, model size, and network latency.

### 2.2 Shared PSRAM Region — The Memory Bottleneck

A single shared PSRAM block (`psram.cpp:10-22`) is **reused sequentially** by different pipeline stages:

```c
void *shared_region = NULL;                    // One block for all stages
std::string sharedMemoryInUseFor = "";         // Soft state tracking — NO MUTEX
```

The shared region (TENSOR_ARENA_SIZE + MAX_MODEL_SIZE bytes) is used by:

| Pipeline Step | PSRAM Usage | State Flag |
|---------------|-------------|------------|
| Take Image (STBI decode) | JPEG→RGB decode buffers | `"TakeImage"` |
| Alignment | Temporary image for template matching | `"Aligning"` |
| CNN Inference | Tensor arena + model weights | `"Digitization_Tensor"` / `"Digitization_Model"` |

Each step claims the region by writing the `sharedMemoryInUseFor` string, then releases it by setting it to `""`. **There is no mutex.** This is safe only because these steps run sequentially on Core 0. But the web server on Core 1 also accesses objects that **read from or encode images stored in this same PSRAM**.

---

## 3. Five Specific Mechanisms That Stall the Web Server

### Mechanism 1: `SendJPGtoHTTP()` — JPEG Encoding On The HTTP Thread

**File:** `CImageBasis.cpp:142-165`

When the web UI requests any ROI image or the alignment image (`/img_tmp/alg.jpg`, `/img_tmp/alg_roi.jpg`, digit/analog ROI images), the handler calls `ClassFlowControll::GetJPGStream()` → `CImageBasis::SendJPGtoHTTP()`.

This function:
1. Calls `RGBImageLock()` — a **polling wait** using `vTaskDelay(1000ms)` up to `_waitmaxsec` times (`CImageBasis.cpp:29-50`)
2. Runs `stbi_write_jpg_to_func()` — **CPU-intensive JPEG compression** of the full VGA image (640×480×3 = 900 KB RGB → ~50-100 KB JPEG). This takes **100–300 ms** on Core 1.
3. Sends chunks via `httpd_resp_send_chunk()` — each chunk can block for up to `send_wait_timeout` (5 seconds) if the TCP window is full.

**During this entire sequence (100–5000+ ms), the httpd task is blocked. No other HTTP request is served.**

### Mechanism 2: Camera Frame Buffer Contention

**File:** `ClassControllCamera.cpp:102, 854-910`

The camera has **only 1 frame buffer** (`fb_count = 1`), allocated in PSRAM.

When the flow task (Core 0) calls `esp_camera_fb_get()` to capture an image for inference, it holds the frame buffer during the STBI JPEG-to-RGB decode (which itself uses the shared PSRAM region).

If a web request for `/img_tmp/raw.jpg` arrives during this window, it calls `CCamera::CaptureToHTTP()` which also calls `esp_camera_fb_get()`. With `fb_count = 1`, the second get **blocks until the first buffer is returned**. The flow task typically holds the buffer for 50–500 ms during decode.

Additionally, both `CaptureToHTTP()` and `CaptureToBasisImage()` use a **double-get pattern**:
```c
camera_fb_t *fb = esp_camera_fb_get();   // Get buffer (forces new capture)
esp_camera_fb_return(fb);                 // Return immediately
fb = esp_camera_fb_get();                 // Get fresh frame
```
With a single frame buffer, the second `esp_camera_fb_get()` must wait for a full camera frame (up to 33 ms at 30 FPS).

### Mechanism 3: `RGBImageLock()` — Race Condition Spin-Wait

**File:** `CImageBasis.cpp:29-50`

The image "lock" mechanism is a **non-atomic boolean flag** with a polling loop:

```c
uint8_t * CImageBasis::RGBImageLock(int _waitmaxsec) {
    if (islocked) {
        for (int i = 0; i <= _waitmaxsec; ++i) {
            vTaskDelay(1000 / portTICK_PERIOD_MS);  // Sleep 1 SECOND per iteration
            if (!islocked) break;
        }
    }
    if (islocked) return NULL;
    return rgb_image;      // ← NOT ATOMIC: does not actually set islocked = true!
}
```

Critical problems:
1. **Not a real lock**: `RGBImageLock()` never sets `islocked = true`. The caller gets the pointer but the flag is never raised. Any other caller also passes through.
2. **1-second granularity**: If the image IS locked, the wait polls every 1 second — adding 1–N seconds of latency to the web response.
3. **Race condition**: Two tasks (flow on Core 0 modifying `rgb_image`, web handler on Core 1 encoding it to JPEG) can access the same buffer simultaneously → **torn frames, corrupted JPEG output**.

### Mechanism 4: `GetJPGStream()` for `alg_roi.jpg` — Heavy Image Operations

**File:** `ClassFlowControll.cpp:687-930`

When the web UI overview page loads, it requests `alg_roi.jpg` which triggers `GetJPGStream("alg_roi.jpg", req)`. Depending on compile options:

**Without `ALGROI_LOAD_FROM_MEM_AS_JPG`:**
1. Allocates a **full copy** of the alignment image: `new CImageBasis("alg_roi", flowalignment->ImageBasis)` — 900 KB PSRAM allocation (`CImageBasis.cpp:263-307`)
2. Calls `flowalignment->DrawRef(_send)` — draws reference point markers
3. Calls `flowdigit->DrawROI(_send)` — draws digit ROI rectangles  
4. Calls `flowanalog->DrawROI(_send)` — draws analog ROI rectangles
5. Calls `_send->SendJPGtoHTTP(req)` — JPEG encodes and sends

Each draw operation iterates all ROIs doing pixel-level manipulation. Then JPEG encoding follows. **Total: 200–600 ms blocking the httpd task.**

**With `ALGROI_LOAD_FROM_MEM_AS_JPG`:**
The code checks `aktstatus` (a shared string modified by Core 0 without synchronization) to decide which static image to serve. During the "Take Image" step, it accesses `flowalignment->AlgROI->data` which is being populated by the flow task on Core 0. **Data race.**

### Mechanism 5: `handler_wasserzaehler()` Full Page — Multiple Blocking Stages

**File:** `MainFlowControl.cpp:564-795`

The `/value?full` endpoint (the main overview page) performs:

1. **`flowctrl.getReadout()`** → reads current value string from post-processing (quick, ~1 ms)
2. **`flowctrl.GetAllDigit()`** → **allocates `HTMLInfo*` objects** containing pointers to ROI images currently being modified by Core 0 during inference
3. **Loop over digit ROIs** → generates HTML with embedded `<img>` tags, sends via `httpd_resp_sendstr_chunk()` — each chunk can block up to 5s
4. **`flowctrl.GetAllAnalog()`** → same pattern
5. **Loop over analog ROIs** → more HTML chunks
6. **Embed `alg_roi.jpg`** reference → triggers browser to make ANOTHER request for the image

The handler itself runs for **10–100 ms** generating HTML. But it triggers **N+1 follow-up image requests** from the browser (one per ROI image + the full alignment image). Each of those image requests hits Mechanism 1 (JPEG encoding), and they are all **serialized by the single-threaded httpd**.

For a typical setup with 8 digit ROIs + 4 analog ROIs:
- 12 ROI image requests × 50–100 ms each = **600–1200 ms**
- 1 `alg_roi.jpg` request = **200–600 ms**
- **Total page load: 1–2 seconds minimum, assuming zero contention**

During inference, add Mechanisms 2–4 and the total rises to **3–15 seconds**.

---

## 4. Concurrency Diagram: The Collision Window

```
Core 0 (task_autodoFlow, priority 2):
 ──[Take Image]──[Alignment]──[Digit CNN]──[Analog CNN]──[Post]──[MQTT]──[Sleep...]
    │  PSRAM:     │  PSRAM:     │  PSRAM:       │
    │  "TakeImage"│  "Aligning" │  "Digitize"   │
    │  Camera FB  │  tmpImage   │  Tensor arena  │
    │  held 50ms  │  modifying  │  + Model       │
    │             │  ImageBasis │  100-500ms/ROI  │
    ▼             ▼             ▼                 ▼
                                                  
Core 1 (httpd, priority 3):
 ──[idle]──[GET /value?full]──────────────────────────────────
              │
              ├── getReadout() ............... 1ms
              ├── GetAllDigit() .............. gets HTMLInfo* with image pointers
              │     └── ROI images point to GENERAL[n]->ROI[i]->image
              │         which is being MODIFIED by Core 0 (doAlignAndCut)  ← DATA RACE
              ├── httpd_resp_sendstr_chunk() . 1-5000ms (TCP backpressure)
              ├── GetAllAnalog() ............. same race
              └── httpd_resp_sendstr_chunk() . 1-5000ms
                     │
                     └── Browser now requests each ROI image:
                         GET /img_tmp/dig1_roi0.jpg  → SendJPGtoHTTP() blocks 100-300ms
                         GET /img_tmp/dig1_roi1.jpg  → blocks 100-300ms  
                         GET /img_tmp/alg_roi.jpg    → blocks 200-600ms
                         ...all SERIALIZED in httpd task...
```

During the `[Digit CNN]` phase, the flow task is:
- Iterating each ROI, resizing images, loading into tensor, calling `interpreter->Invoke()`
- **Modifying `GENERAL[n]->ROI[i]->image` and `result_float`/`result_klasse`**  

If a web handler is simultaneously reading these same structures via `GetAllDigit()`/`GetAllAnalog()`, it reads **partially-updated data** without any synchronization.

---

## 5. Shared State Without Synchronization

| Global Variable | Location | Writer (Core 0) | Reader (Core 1) | Protection |
|----------------|----------|-----------------|-----------------|------------|
| `flowctrl` (ClassFlowControll) | `MainFlowControl.cpp:38` | `doFlow()` | All web handlers | **NONE** |
| `flowisrunning` | `MainFlowControl.cpp:46` | `doflow()` | `handler_flow_start` | **Bool, not atomic** |
| `aktstatus` | `ClassFlowControll.cpp` member | `doFlow()` loop | `handler_statusflow`, `GetJPGStream` | **NONE** |
| `countRounds` | `MainFlowControl.cpp:51` | `task_autodoFlow` | `handler_openmetrics`, `handler_wasserzaehler` | **NONE** |
| `sharedMemoryInUseFor` | `psram.cpp:12` | Flow pipeline steps | Potentially web (STBI path) | **NONE** |
| `CCstatus` | `ClassControllCamera.cpp:150` | Camera config | Web handlers | **NONE** |
| `CFstatus` | `MainFlowControl.cpp:40` | Flow init | Web handlers | **NONE** |
| `GENERAL[n]->ROI[i]->image` | `ClassFlowCNNGeneral` | `doAlignAndCut`, `doNeuralNetwork` | `GetJPGStream`, `GetAllDigit` | **NONE** |
| `GENERAL[n]->ROI[i]->result_float` | `ClassFlowCNNGeneral` | `doNeuralNetwork` | `getReadout`, `GetAllDigit` | **NONE** |

---

## 6. Worst-Case Timeline

Assume: User opens web overview page (`/value?full`) during a CNN inference round.

```
T+0ms      Browser sends GET /value?full
T+1ms      httpd dispatches handler_wasserzaehler()
T+2ms      Handler calls flowctrl.GetAllDigit()
           → Returns HTMLInfo* pointers to live ROI images
           → Core 0 is mid-inference, modifying these images
T+5ms      Handler starts building HTML, calls httpd_resp_sendstr_chunk()
T+10ms     TCP send buffer full (WiFi slow) → blocks up to 5000ms
T+5010ms   Chunk sent. Handler builds more HTML, sends next chunk.
T+5020ms   Response complete. Browser starts loading embedded images.

T+5021ms   Browser: GET /img_tmp/dig1_roi0.jpg
T+5022ms   httpd: GetJPGStream() → finds image, calls SendJPGtoHTTP()
T+5023ms   RGBImageLock() — image is being written by Core 0 (doAlignAndCut for next ROI)
           BUT: islocked is false (nobody sets it!) → proceeds anyway
T+5023ms   stbi_write_jpg_to_func() starts encoding WHILE Core 0 modifies pixels
           → Corrupted JPEG or crash
T+5200ms   JPEG encoding complete, sends response

T+5201ms   Browser: GET /img_tmp/dig1_roi1.jpg
           ...same pattern, 100-300ms each...

T+6500ms   Browser: GET /img_tmp/alg_roi.jpg
T+6501ms   Allocates full image copy (900KB PSRAM)
           → If Core 0 is in "Digitization" phase using shared PSRAM
           → heap_caps_malloc may fail or fragment PSRAM
T+6502ms   DrawROI() iterates all ROIs drawing rectangles
T+6700ms   SendJPGtoHTTP() encodes + sends
T+7000ms   → Page finally rendered (if no errors)

TOTAL: ~2-7 seconds to load the overview page
WORST CASE with TCP backpressure: 15-25 seconds
```

---

## 7. Root Causes Ranked by Impact

### Primary: Single-Threaded HTTP Handler Execution
The ESP-IDF httpd processes one handler at a time. Any handler that does CPU work (JPEG encoding), I/O (SD card file reads), or network sends (chunked HTTP responses) blocks ALL other HTTP requests for the full duration.

### Secondary: No Synchronization Between Flow Task and Web Handlers
The flow task and web handlers share mutable state (images, results, status strings) without any mutex, semaphore, or atomic variable. The `RGBImageLock()` mechanism is non-functional (it never actually locks). This leads to:
- Torn image data during JPEG encoding
- Inconsistent readout values
- Potential crashes from reading freed/reallocated memory

### Tertiary: PSRAM Contention
The single-frame-buffer camera and the shared PSRAM region create resource contention between capture, inference, and web serving. With only 4 MB of PSRAM, the model weights (~1-2 MB), tensor arena (~1-2 MB), frame buffer (~300 KB), and image copies (~900 KB each) leave minimal headroom for concurrent operations.

### Quaternary: `CaptureToStream()` — Infinite Blocking
The MJPEG stream handler (`handler_stream`) enters an **infinite loop** (`ClassControllCamera.cpp:834`) that only exits when the client disconnects. While active, **the entire httpd task is blocked** — no other web page, API call, or image request can be served.

---

## 8. Recommendations

1. **Add a proper FreeRTOS mutex** around the shared `flowctrl` state. At minimum, protect the `GENERAL[n]->ROI[i]` vectors and the `flowalignment->ImageBasis` pointer with a `SemaphoreHandle_t`.

2. **Double-buffer the results**: Make the flow task write to a "back buffer" and swap a pointer atomically when the round completes. Web handlers always read from the "front buffer" which is stable.

3. **Move JPEG encoding off the httpd task**: Either pre-encode images at the end of each flow round (and serve from a PSRAM/SD cache), or spawn a short-lived encoding task.

4. **Fix `RGBImageLock()`**: Either remove it (it does nothing useful) or replace it with a real FreeRTOS mutex (`xSemaphoreTake`/`xSemaphoreGive`).

5. **Serve the MJPEG stream from a separate httpd instance** (on a separate port/task) so it doesn't block the main web UI.

6. **Pre-render `alg_roi.jpg`** at end of each flow round instead of constructing it on-demand per web request. The `ALGROI_LOAD_FROM_MEM_AS_JPG` compile option partially does this but has its own race conditions.

7. **Increase `fb_count` to 2** if PSRAM permits, to eliminate camera frame buffer contention between the flow task and web raw image requests.
