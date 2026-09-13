/* Envoy Framework - the ABI of a state provider. Version 1.
 *
  * A provider is an ordinary DLL in SKSE\Plugins\EnvoyProviders\. The bridge
  * walks the folder, asks EnvoyProvider_GetInfo and builds a map "key ->
  * provider". No mod - no DLL - the key answers ENVOY_S_NO_PROVIDER. Not a
  * single check of the edition of the game inside the bridge.
 *
  * The engines of recognition and synthesis do NOT belong here: they attach to
  * the service of models, not to the plugin. The plugin has no models and no
  * microphone.
 */
#ifndef ENVOY_ABI_H
#define ENVOY_ABI_H

#include <stdint.h>

#define ENVOY_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef enum EnvoyType {
    ENVOY_T_BOOL   = 1,
    ENVOY_T_INT    = 2,
    ENVOY_T_FLOAT  = 3,
    ENVOY_T_STRING = 4,
    ENVOY_T_FORM   = 5
} EnvoyType;

typedef enum EnvoyStatus {
    ENVOY_S_NO_PROVIDER = 0,
    ENVOY_S_OK          = 1,
    ENVOY_S_FAILED      = 2
} EnvoyStatus;

typedef enum EnvoyCost {
    ENVOY_COST_CHEAP     = 0,  /* worked out when the snapshot is built */
    ENVOY_COST_EXPENSIVE = 1   /* on request only, with a cache */
} EnvoyCost;

typedef struct EnvoyKeyDecl {
    const char* key;          /* the full name, namespace included */
    uint32_t    type;         /* EnvoyType */
    uint32_t    cost;         /* EnvoyCost */
    float       ttlSec;       /* 0 - never stales */
    const char* description;  /* ends up in GetKeys() */
} EnvoyKeyDecl;

typedef struct EnvoyValue {
    uint32_t    type;
    int32_t     i;       /* ENVOY_T_BOOL, ENVOY_T_INT */
    float       f;       /* ENVOY_T_FLOAT */
    const char* str;     /* ENVOY_T_STRING, lives until the snapshot is finished */
    uint32_t    formId;  /* ENVOY_T_FORM */
} EnvoyValue;

typedef struct EnvoyProviderInfo {
    uint32_t            abiVersion;   /* ENVOY_ABI_VERSION */
    const char*         id;           /* "higgs", "vrik", "mock" */
    const char*         displayName;
    uint32_t            keyCount;
    const EnvoyKeyDecl* keys;
} EnvoyProviderInfo;

/* --- the exports a provider must have --- */

__declspec(dllexport) const EnvoyProviderInfo* EnvoyProvider_GetInfo(void);

/* configJson - the section of the settings of the bridge for this provider; may
   be NULL. Gives back 0 on success. */
__declspec(dllexport) int  EnvoyProvider_Init(const char* configJson);

__declspec(dllexport) void EnvoyProvider_Shutdown(void);

/* Called from the main thread while the snapshot is built. Must return quickly
   and must never block. Gives back an EnvoyStatus. */
__declspec(dllexport) int  EnvoyProvider_Read(const char* key, EnvoyValue* out);

#ifdef __cplusplus
}
#endif
#endif /* ENVOY_ABI_H */
