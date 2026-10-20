#pragma once

#include <stdbool.h>

#include "android/utils/compiler.h"

ANDROID_BEGIN_HEADER

// Create a sparse, empty file. Existing files are truncated.
bool android_createSparseFile(const char* path);

// Copy bytes into a new sparse file, preserving the logical length.
// The destination must not exist.
bool android_copySparseFile(const char* source, const char* destination);

// Publish a completed image, replacing an existing destination if necessary.
bool android_replaceImageFile(const char* source, const char* destination);

ANDROID_END_HEADER
