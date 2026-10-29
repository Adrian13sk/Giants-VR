// Writing small state files (settings, saves) so that a full disk, an I/O error or a power cut never leaves a good
// file replaced by an empty or half-written one: write a temporary, check it, flush it to the storage, rename.
#pragma once

#include <string>

namespace gvr {

// Writes `text` to `path` atomically (via `path`.tmp); false (and the old file untouched) on any failure.
bool writeFileAtomic(const std::string& path, const std::string& text);

}  // namespace gvr
