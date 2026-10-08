#pragma once

#include <string>

namespace home2hook {

    // True if data begins with the zstd frame magic
    bool IsZstdFrame(const std::string& data);

    // Wrap raw bytes in a valid zstd frame built from uncompressed raw blocks
    // The game's zstd decoder returns the exact input, so a glb wrapped this way decompresses w/o loss.
    // There is no size reduction, but it needs no compression library, which at least keeps the backend with 1 less dependency.
    std::string ZstdWrapRaw(const std::string& data);

}
