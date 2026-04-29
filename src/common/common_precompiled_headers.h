// Copyright 2022 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.
#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef CreateMutex
#undef CreateEvent
#undef CreateProcess
#undef CreateSemaphore
#undef DeleteFile
#undef CreateFile
#undef CreateDirectory
#undef REMOVE
#undef DELETE
#undef ERROR
#undef OUT
#undef IN
#undef far
#undef near
#undef pascal
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <fmt/format.h>
#include "common/assert.h"
#include "common/common_types.h"