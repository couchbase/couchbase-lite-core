//
// Created by Jens Alfke on 3/11/25.
//

#include "UUID.hh"
#include "c4DatabaseTypes.h"  // for C4UUID
#include "Error.hh"
#include "SecureDigest.hh"
#include "SecureRandomize.hh"
#include "slice_stream.hh"

namespace litecore {
    using namespace std;
    using namespace fleece;

    static_assert(UUID::Size == sizeof(UUID));
    static_assert("70EA9E91-C689-4789-8E10-901D8E55EDBE"_uuid.data()[0] == 0x70);
    static_assert("70EA9E91-C689-4789-8E10-901D8E55EDBE"_uuid.data()[15] == 0xBE);

    UUID UUID::generateRandom() {
        // https://en.wikipedia.org/wiki/Universally_unique_identifier#Version_4_.28random.29
        UUID uuid;
        SecureRandomize(mutable_slice(slice(uuid)));
        uuid.stampVersion(4);
        return uuid;
    }

    UUID UUID::generateNamespaced(UUID const& namespaceUUID, fleece::slice name) {
        // https://datatracker.ietf.org/doc/html/rfc9562#name-uuid-version-5
        UUID uuid;
        SHA1 digest = (SHA1Builder{} << slice(namespaceUUID) << name).finish();
        memcpy(uuid._bytes.data(), &digest, Size);  // copy first 128 bits of SHA-1
        uuid.stampVersion(5);
        return uuid;
    }

    std::optional<UUID> UUID::parse(std::string_view str) {
        UUID uuid;
        if ( parse(str, uuid) ) return uuid;
        else
            return nullopt;
    }

    UUID::UUID(fleece::slice bytes) {
        Assert(bytes.size == Size);
        ::memcpy(_bytes.data(), bytes.buf, Size);
    }

    std::string UUID::to_string() const {
        string str = slice(this, Size).hexString();
        for ( size_t pos = 20; pos >= 8; pos -= 4 ) str.insert(pos, "-");
        return str;
    }

    std::strong_ordering operator<=>(UUID const& a, UUID const& b) {
        if ( int cmp = memcmp(a.data(), b.data(), UUID::Size); cmp < 0 ) return strong_ordering::less;
        else if ( cmp > 0 )
            return strong_ordering::greater;
        else
            return strong_ordering::equal;
    }

    void UUID::stampVersion(uint8_t version) {
        _bytes[6] = (_bytes[6] & ~0xF0) | uint8_t(version << 4);
        _bytes[8] = (_bytes[8] & ~0xC0) | 0x80;
    }

}  // namespace litecore
