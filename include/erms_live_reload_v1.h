#ifndef ERMS_LIVE_RELOAD_V1_H
#define ERMS_LIVE_RELOAD_V1_H

/* Optional, opt-in live-reload ABI for DLLs whose INI appears in NativeTweaks.
 * The Hub calls these exports only after the INI has been saved successfully. */

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#define ERMS_RELOAD_CALL __cdecl
#define ERMS_RELOAD_EXPORT __declspec(dllexport)
#else
#define ERMS_RELOAD_CALL
#define ERMS_RELOAD_EXPORT __attribute__((dllexport))
#endif

#define ERMS_RELOAD_PENDING_V1 0u
#define ERMS_RELOAD_APPLIED_V1 1u
#define ERMS_RELOAD_REJECTED_V1 2u
#define ERMS_RELOAD_RESTART_REQUIRED_V1 3u
#define ERMS_RELOAD_FAILED_V1 4u

/* Return a nonzero request ID, or zero if the request cannot be queued.
 * ini_path is a null-terminated UTF-16 path to the saved INI. */
ERMS_RELOAD_EXPORT uint32_t ERMS_RELOAD_CALL
NativeTweaks_RequestReloadV1(const wchar_t* ini_path);

/* Return one ERMS_RELOAD_* value. Keep request results queryable for at least
 * five seconds so the Hub can observe completion without blocking the menu. */
ERMS_RELOAD_EXPORT uint32_t ERMS_RELOAD_CALL
NativeTweaks_GetReloadStatusV1(uint32_t request_id);

#ifdef __cplusplus
}
#endif

#endif /* ERMS_LIVE_RELOAD_V1_H */
