// snapshot_zip.h — Walk a directory and produce an in-RAM zip blob via miniz.
//
// Pure filesystem + compression. Does NOT include any Kenshi header — safe
// to run from a background thread alongside Kenshi's render/update threads.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kmp {

/// Recursively zip every regular file under `abs_path` into an in-memory
/// zip archive. Entry names are stored as paths relative to `abs_path`
/// using forward slashes.
///
/// Returns an empty vector on any failure (missing dir, miniz error, etc.).
/// Logs nothing — caller decides what to report.
std::vector<uint8_t> zip_directory(const std::string& abs_path);

} // namespace kmp
