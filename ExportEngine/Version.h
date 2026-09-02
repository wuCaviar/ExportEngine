#pragma once

#include "ExportEngine.h"
#include <cstdint>
#include <string>

// ---- 语义版本 (手动维护) ----
#define EE_VERSION_MAJOR 0
#define EE_VERSION_MINOR 1
#define EE_VERSION_PATCH 0

// DLL 导出 C 接口 (C linkage, outside namespace)
extern "C" {
EE_API const char *EE_getVersion();
EE_API const char *EE_getBuildTimestamp();
}
