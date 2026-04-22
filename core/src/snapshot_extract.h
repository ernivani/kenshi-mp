// snapshot_extract.h — miniz-based zip-to-dir extractor.
#pragma once

#include <string>

namespace kmp {

/// Open `zip_path`, read every file entry, write under `dst_dir`
/// (creating parent dirs as needed). Returns false on any miniz or
/// filesystem error.
bool extract_zip_to_dir(const std::string& zip_path,
                        const std::string& dst_dir);

} // namespace kmp
