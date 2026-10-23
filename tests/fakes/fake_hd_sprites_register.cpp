#include "pch.h"

// The farload/buffer tests do not exercise HD sprite composition.
namespace hd_sprites {
    void Register_Load(const void*, int32_t, const char*, int32_t) {
    }
} // namespace hd_sprites
