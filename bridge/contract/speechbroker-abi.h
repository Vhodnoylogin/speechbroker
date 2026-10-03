/* Speech Broker - the ABI of a state provider. Version 1.
 *
  * A provider is an ordinary DLL in SKSE\Plugins\SpeechBrokerProviders\. The bridge
  * walks the folder, asks SpeechBrokerProvider_GetInfo and builds a map "key ->
  * provider". No mod - no DLL - the key answers SPEECHBROKER_S_NO_PROVIDER. Not a
  * single check of the edition of the game inside the bridge.
 *
  * The engines of recognition and synthesis do NOT belong here: they attach to
  * the service of models, not to the plugin. The plugin has no models and no
  * microphone.
 */
#ifndef SPEECHBROKER_ABI_H
#define SPEECHBROKER_ABI_H

#include <stdint.h>

#define SPEECHBROKER_ABI_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SpeechBrokerType {
    SPEECHBROKER_T_BOOL   = 1,
    SPEECHBROKER_T_INT    = 2,
    SPEECHBROKER_T_FLOAT  = 3,
    SPEECHBROKER_T_STRING = 4,
    SPEECHBROKER_T_FORM   = 5
} SpeechBrokerType;

typedef enum SpeechBrokerStatus {
    SPEECHBROKER_S_NO_PROVIDER = 0,
    SPEECHBROKER_S_OK          = 1,
    SPEECHBROKER_S_FAILED      = 2
} SpeechBrokerStatus;

typedef enum SpeechBrokerCost {
    SPEECHBROKER_COST_CHEAP     = 0,  /* worked out when the snapshot is built */
    SPEECHBROKER_COST_EXPENSIVE = 1   /* on request only, with a cache */
} SpeechBrokerCost;

typedef struct SpeechBrokerKeyDecl {
    const char* key;          /* the full name, namespace included */
    uint32_t    type;         /* SpeechBrokerType */
    uint32_t    cost;         /* SpeechBrokerCost */
    float       ttlSec;       /* 0 - never stales */
    const char* description;  /* ends up in GetKeys() */
} SpeechBrokerKeyDecl;

typedef struct SpeechBrokerValue {
    uint32_t    type;
    int32_t     i;       /* SPEECHBROKER_T_BOOL, SPEECHBROKER_T_INT */
    float       f;       /* SPEECHBROKER_T_FLOAT */
    const char* str;     /* SPEECHBROKER_T_STRING, lives until the snapshot is finished */
    uint32_t    formId;  /* SPEECHBROKER_T_FORM */
} SpeechBrokerValue;

typedef struct SpeechBrokerProviderInfo {
    uint32_t            abiVersion;   /* SPEECHBROKER_ABI_VERSION */
    const char*         id;           /* "higgs", "vrik", "mock" */
    const char*         displayName;
    uint32_t            keyCount;
    const SpeechBrokerKeyDecl* keys;
} SpeechBrokerProviderInfo;

/* --- the exports a provider must have --- */

__declspec(dllexport) const SpeechBrokerProviderInfo* SpeechBrokerProvider_GetInfo(void);

/* configJson - the section of the settings of the bridge for this provider; may
   be NULL. Gives back 0 on success. */
__declspec(dllexport) int  SpeechBrokerProvider_Init(const char* configJson);

__declspec(dllexport) void SpeechBrokerProvider_Shutdown(void);

/* Called from the main thread while the snapshot is built. Must return quickly
   and must never block. Gives back an SpeechBrokerStatus. */
__declspec(dllexport) int  SpeechBrokerProvider_Read(const char* key, SpeechBrokerValue* out);

#ifdef __cplusplus
}
#endif
#endif /* SPEECHBROKER_ABI_H */
