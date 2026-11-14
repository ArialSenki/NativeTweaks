#ifndef ERMS_PROVIDER_API_V1_H
#define ERMS_PROVIDER_API_V1_H

/* Superseded, unimplemented proposal. The current client does not export
 * ERMS_GetHostApiV1; see docs/PROVIDER_API_V1.md before considering use. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ERMS_PROVIDER_ABI_V1 1u
#define ERMS_HOST_MODULE_NAME_W L"NativeTweaks.dll"
#define ERMS_HOST_API_EXPORT "ERMS_GetHostApiV1"

#define ERMS_MAX_PROVIDERS_V1 32u
#define ERMS_MAX_SETTINGS_PER_PROVIDER_V1 64u
#define ERMS_MAX_ID_BYTES_V1 64u
#define ERMS_MAX_LABEL_BYTES_V1 96u
#define ERMS_MAX_DESCRIPTION_BYTES_V1 256u
#define ERMS_MAX_CHOICES_PER_SETTING_V1 32u

#if defined(_MSC_VER)
#define ERMS_CALL __cdecl
#else
#define ERMS_CALL
#endif

typedef enum ERMS_ValueKindV1 {
    ERMS_VALUE_BOOL_V1 = 1,
    ERMS_VALUE_FLOAT_V1 = 2,
    ERMS_VALUE_CHOICE_V1 = 3
} ERMS_ValueKindV1;

typedef enum ERMS_ScaleModeV1 {
    ERMS_SCALE_NONE_V1 = 0,    /* Required for booleans and choices. */
    ERMS_SCALE_LINEAR_V1 = 1,  /* Default for ordinary bounded numeric settings. */
    ERMS_SCALE_LOG1P_V1 = 2    /* Nonnegative wide ranges, mapped through log(1 + value). */
} ERMS_ScaleModeV1;

typedef enum ERMS_SetResultV1 {
    ERMS_SET_APPLIED_V1 = 0,
    ERMS_SET_RESTART_REQUIRED_V1 = 1,
    ERMS_SET_REJECTED_V1 = 2,
    ERMS_SET_FAILED_V1 = 3
} ERMS_SetResultV1;

typedef enum ERMS_SettingFlagsV1 {
    ERMS_SETTING_NONE_V1 = 0u,
    ERMS_SETTING_RESTART_REQUIRED_V1 = 1u << 0
} ERMS_SettingFlagsV1;

typedef struct ERMS_ValueV1 {
    uint32_t struct_size;
    uint32_t kind;
    union {
        uint32_t boolean_value; /* 0 or 1 */
        float float_value;
        uint32_t choice_index;
    } data;
} ERMS_ValueV1;

typedef struct ERMS_ChoiceV1 {
    const char *label_utf8;
} ERMS_ChoiceV1;

typedef struct ERMS_SettingV1 {
    uint32_t struct_size;
    uint32_t kind;
    const char *id_utf8;
    const char *label_utf8;
    const char *description_utf8;
    ERMS_ValueV1 default_value;
    float minimum;
    float maximum;
    float step;                /* One left/right adjustment; slider values snap to this increment. */
    uint32_t scale_mode;       /* ERMS_ScaleModeV1; only applies to ERMS_VALUE_FLOAT_V1. */
    uint32_t choice_count;
    const ERMS_ChoiceV1 *choices;
    uint32_t flags;
} ERMS_SettingV1;

typedef struct ERMS_ProviderV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    const char *id_utf8;
    const char *display_name_utf8;
    const char *description_utf8;
    uint32_t setting_count;
    const ERMS_SettingV1 *settings;
    void *context;
    int32_t (ERMS_CALL *get_value)(
        void *context,
        const char *setting_id_utf8,
        ERMS_ValueV1 *out_value);
    ERMS_SetResultV1 (ERMS_CALL *set_value)(
        void *context,
        const char *setting_id_utf8,
        const ERMS_ValueV1 *value);
    ERMS_SetResultV1 (ERMS_CALL *reset_value)(
        void *context,
        const char *setting_id_utf8);
} ERMS_ProviderV1;

typedef struct ERMS_HostApiV1 {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t (ERMS_CALL *register_provider)(const ERMS_ProviderV1 *provider);
} ERMS_HostApiV1;

typedef int32_t (ERMS_CALL *ERMS_GetHostApiV1Fn)(
    uint32_t requested_abi,
    ERMS_HostApiV1 *out_api,
    uint32_t out_api_size);

#ifdef __cplusplus
}
#endif

#endif /* ERMS_PROVIDER_API_V1_H */
