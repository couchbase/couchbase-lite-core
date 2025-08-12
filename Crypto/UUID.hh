//
// Created by Jens Alfke on 3/11/25.
//

#pragma once
#include "fleece/slice.hh"
#include <array>
#include <compare>
#include <cstring>  // for memcpy
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

struct C4UUID;  // declared in c4DatabaseTypes.h

namespace litecore {

    /** A standard 128-bit UUID conforming to RFC 9562. Instances are immutable. */
    class UUID {
      public:
        static constexpr size_t Size = 16;

        /// Generates a securely-random (type 4) UUID.
        static UUID generateRandom();

        /// Generates a deterministic type-5 UUID from a namespace and an arbitrary name.
        static UUID generateNamespaced(UUID const& namespaceUUID, fleece::slice name);

        /// Parses a UUID from the standard hex string form. Dashes are allowed but ignored.
        static std::optional<UUID> parse(std::string_view);

        /// Constructs a UUID from 16 bytes.
        constexpr explicit UUID(std::span<const uint8_t, Size> bytes) {
            std::copy_n(bytes.data(), Size, _bytes.data());
        }

        /// Constructs a UUID from a slice. Asserts that its size is 16.
        explicit UUID(fleece::slice bytes);

        /// Constructs a UUID from a hex string.
        constexpr explicit UUID(const char* str);

        // interoperability with C4UUID
        UUID(C4UUID const& c) : UUID(fleece::slice(&c, Size)) {}

        C4UUID const& asC4UUID() const { return *reinterpret_cast<const C4UUID*>(this); }

        operator C4UUID const&() const { return asC4UUID(); }

        /// A pointer to the data bytes
        constexpr uint8_t const* data() const { return _bytes.data(); }

        /// Size of the data
        constexpr size_t size() const { return Size; }

        /// Access to the bytes as a slice
        [[nodiscard]] constexpr fleece::slice asSlice() const LIFETIMEBOUND { return {_bytes.data(), Size}; }

        explicit constexpr operator fleece::slice() const LIFETIMEBOUND { return asSlice(); }

        /// Encodes the UUID as a standard hex string with dashes in it.
        std::string to_string() const;

        /// Comparison
        friend std::strong_ordering operator<=>(UUID const&, UUID const&);
        friend bool                 operator==(UUID const&, UUID const&) = default;
        friend constexpr UUID       operator""_uuid(const char* str, size_t len);

      private:
        constexpr UUID() = default;  // Note: does not initialize _bytes
        void                  stampVersion(uint8_t version);
        static constexpr bool parse(std::string_view str, UUID& uuid);

        std::array<uint8_t, Size> _bytes;
    };

    //-------- Implementation gunk

    constexpr bool UUID::parse(std::string_view str, UUID& uuid) {
        auto _digittoint = [](int8_t ch) {
            int d = ch - '0';
            if ( (unsigned)d < 10 ) return d;
            d = ch - 'a';
            if ( (unsigned)d < 6 ) return d + 10;
            d = ch - 'A';
            if ( (unsigned)d < 6 ) return d + 10;
            return -1;
        };

        auto   cp = str.begin(), end = str.end();
        size_t dst = 0;
        while ( dst < Size ) {
            if ( cp == end ) return false;
            uint8_t c = *cp++;
            if ( int digit1 = _digittoint(c); digit1 >= 0 ) {
                if ( int digit2 = _digittoint(*cp++); digit2 >= 0 )
                    uuid._bytes[dst++] = uint8_t((digit1 << 4) | digit2);
                else
                    return false;
            } else if ( c != '-' ) {
                return false;
            }
        }
        return (cp == end);
    }

    constexpr UUID::UUID(const char* str) {
        if ( !parse(str, *this) ) throw std::invalid_argument("Invalid UUID string");
    }

    constexpr UUID operator""_uuid(const char* str, size_t len) {
        UUID uuid;
        if ( !UUID::parse({str, len}, uuid) ) throw std::invalid_argument("Invalid UUID string");
        return uuid;
    }

}  // namespace litecore

// Makes UUID hashable, for use in unordered_map:
template <>
struct std::hash<litecore::UUID> {
    std::size_t operator()(litecore::UUID const& id) const noexcept FLPURE {
        std::size_t h;
        ::memcpy(&h, &id, sizeof(h));
        return h;
    }
};
