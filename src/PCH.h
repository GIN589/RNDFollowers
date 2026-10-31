#pragma once

// Windows headers define min/max/near/far macros that break the standard library and our code
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "RE/Skyrim.h"
#include "REL/Relocation.h"
#include "SKSE/SKSE.h"

#include <spdlog/sinks/basic_file_sink.h>

using namespace std::literals;
