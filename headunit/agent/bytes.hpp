// SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
// Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
#ifndef NBT_AGENT_BYTES_HPP
#define NBT_AGENT_BYTES_HPP

// Where bytes come from and where they go, for the package reader and the code around it.
// QNX 6.5 / GCC 4.4: no lambdas, nullptr or NSDMI.

#include <stddef.h>
#include <stdint.h>

namespace nbt {

// Where an archive's bytes come from: a file in staging or on a USB stick, or memory in the tests.
class ByteSource {
public:
    virtual ~ByteSource() {}
    virtual uint64_t size() const = 0;
    // Exactly `len` bytes from `offset`; false if they cannot all be read.
    virtual bool read(uint64_t offset, void* out, size_t len) = 0;
};

// Where an extracted file goes.
class ByteSink {
public:
    virtual ~ByteSink() {}
    virtual bool write(const void* data, size_t len) = 0;
};

}  // namespace nbt

#endif
