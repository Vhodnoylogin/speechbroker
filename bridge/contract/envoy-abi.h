/* Envoy Framework — ABI поставщика состояния. Версия 1.
 *
 * Поставщик — обычная DLL в SKSE\Plugins\EnvoyProviders\. Мост перебирает папку,
 * спрашивает EnvoyProvider_GetInfo и строит карту "ключ -> поставщик".
 * Нет мода — нет DLL — ключ отвечает ENVOY_S_NO_PROVIDER. Ни одной проверки
 * редакции игры внутри моста.
 *
 * Движки распознавания и синтеза сюда НЕ входят: они подключаются к службе
 * моделей, а не к плагину. У плагина нет моделей и нет микрофона.
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
    ENVOY_COST_CHEAP     = 0,  /* считается при сборке снимка */
    ENVOY_COST_EXPENSIVE = 1   /* только по запросу, с кэшем */
} EnvoyCost;

typedef struct EnvoyKeyDecl {
    const char* key;          /* полное имя, включая пространство имён */
    uint32_t    type;         /* EnvoyType */
    uint32_t    cost;         /* EnvoyCost */
    float       ttlSec;       /* 0 - не устаревает */
    const char* description;  /* попадает в GetKeys() */
} EnvoyKeyDecl;

typedef struct EnvoyValue {
    uint32_t    type;
    int32_t     i;       /* ENVOY_T_BOOL, ENVOY_T_INT */
    float       f;       /* ENVOY_T_FLOAT */
    const char* str;     /* ENVOY_T_STRING, живёт до конца сборки снимка */
    uint32_t    formId;  /* ENVOY_T_FORM */
} EnvoyValue;

typedef struct EnvoyProviderInfo {
    uint32_t            abiVersion;   /* ENVOY_ABI_VERSION */
    const char*         id;           /* "higgs", "vrik", "mock" */
    const char*         displayName;
    uint32_t            keyCount;
    const EnvoyKeyDecl* keys;
} EnvoyProviderInfo;

/* --- экспорты, обязательные для поставщика --- */

__declspec(dllexport) const EnvoyProviderInfo* EnvoyProvider_GetInfo(void);

/* configJson - раздел конфигурации моста для этого поставщика; может быть NULL.
   Возвращает 0 при успехе. */
__declspec(dllexport) int  EnvoyProvider_Init(const char* configJson);

__declspec(dllexport) void EnvoyProvider_Shutdown(void);

/* Вызывается из главного потока при сборке снимка. Обязан вернуться быстро
   и никогда не блокировать. Возвращает EnvoyStatus. */
__declspec(dllexport) int  EnvoyProvider_Read(const char* key, EnvoyValue* out);

#ifdef __cplusplus
}
#endif
#endif /* ENVOY_ABI_H */
