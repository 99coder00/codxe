#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace t4ff
{
// SHA-256, for content keys of the caches.
class Sha256
{
  public:
    Sha256();
    void update(const void *data, size_t size);
    void update(const std::string &s)
    {
        update(s.data(), s.size());
    }
    std::string hex(); // the digest in hexadecimal (finishes the hash)

  private:
    uint32_t h[8];
    uint8_t block[64];
    size_t used = 0;
    uint64_t length = 0;
    void compress(const uint8_t *p);
};
} // namespace t4ff
