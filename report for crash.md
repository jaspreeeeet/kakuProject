# ESP32 Crash/Reset Risk Audit Report

**Date:** March 5, 2026

## Project: ESP32 Tamagotchi Client (esp32_sketch_test.ino)

---

### 1. Purpose
This report documents all code patterns, functions, and logic in the ESP32 sketch that can trigger a crash, reset, or watchdog timeout. It also highlights memory and synchronization risks that could impact system stability.

---

### 2. Intentional Reset/Crash Triggers
- **ESP.restart()**
  - Used for:
    - WiFi configuration changes
    - OTA update completion
    - Low heap memory detected
    - Daily maintenance reboot
  - All uses are intentional and documented in the code.

- **delay() / vTaskDelay()**
  - Used for timing and task scheduling.
  - Excessive or blocking delays in critical sections can risk watchdog timeouts.

---

### 3. Watchdog & Deadlock Risks
- **portMAX_DELAY**
  - Used in semaphore/mutex waits.
  - If a task waits forever (deadlock), the watchdog will trigger a reset.
- **Mutex/Semaphore Deadlocks**
  - Cross-core synchronization uses mutexes.
  - Risk: If a task never releases a mutex, other tasks can block indefinitely, leading to watchdog resets.

---

### 4. Memory Management Risks
- **malloc / ps_malloc / free**
  - Dynamic memory allocation is used for image/audio buffers and HTTP payloads.
  - If allocation fails (returns NULL), code attempts to handle it, but repeated failures can cause resets.
- **String Concatenation**
  - Used for HTTP payloads and HTML generation.
  - Risk: Large or repeated concatenations can fragment heap and increase allocation failure risk.

---

### 5. Other Observations
- **No inclusion of qr_wifi_setup.h**
  - Not used in the main sketch; no impact on reset/crash logic.
- **all_pets.h**
  - Contains only animation data and delay constants; no logic or reset triggers.

---

### 6. Summary Table
| Trigger Type         | Location/Function         | Risk Level | Notes                                 |
|---------------------|--------------------------|------------|---------------------------------------|
| ESP.restart()       | WiFi/OTA/Heap/Maintenance| Low        | Intentional, for safety/maintenance   |
| delay/vTaskDelay    | Various                  | Medium     | Excessive use can risk watchdog reset |
| portMAX_DELAY       | Mutex/Semaphore waits    | High       | Can deadlock, watchdog will reset     |
| malloc/ps_malloc    | Image/Audio/HTTP buffers | Medium     | Allocation failure can cause resets   |
| String concat       | HTTP/HTML generation     | Low        | Heap fragmentation risk               |

---

### 7. Recommendations
- Review all uses of portMAX_DELAY and ensure timeouts are used where possible.
- Monitor heap usage and fragmentation, especially after OTA or large data transfers.
- Consider adding logging for all resets (reason, heap state, task states).
- Regularly test for mutex deadlocks and memory leaks.

---

**End of Report**
