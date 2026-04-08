#pragma once

#include <cstdint>
#include <libff/algebra/curves/alt_bn128/alt_bn128_pp.hpp>

namespace xrpl {

constexpr std::size_t G1_LENGTH = 64;
constexpr std::size_t G2_LENGTH = 128;
constexpr std::size_t IC_LENGTH = 64;
constexpr std::size_t SCALAR_LENGTH = 32;
constexpr std::size_t PAIR_LENGTH = G1_LENGTH + G2_LENGTH;
constexpr std::size_t GROTH16_PAIR_LENGTH = 4 * (G1_LENGTH + G2_LENGTH);

inline void
be32_to_le32(uint8_t const* be, uint8_t* le)
{
    for (int i = 0; i < 32; ++i)
        le[i] = be[31 - i];
}

inline void
uncompressed_be_to_le(uint8_t const in_be[64], uint8_t out_le[64])
{
    be32_to_le32(in_be + 0, out_le + 0);
    be32_to_le32(in_be + 32, out_le + 32);
}

void
le32_to_bigint_q(
    uint8_t const le[32],
    libff::bigint<libff::alt_bn128_q_limbs>& out);

bool
be32_to_bigint_r(
    const uint8_t be[32],
    libff::bigint<libff::alt_bn128_r_limbs>& out);

bool
g1_from_uncompressed_be(uint8_t const in_be[64], libff::alt_bn128_G1& P);

bool
g1_to_uncompressed_be(libff::alt_bn128_G1 const& point, uint8_t* out_be64);

bool
g2_from_uncompressed_be(uint8_t const in_be[128], libff::alt_bn128_G2& P);

}  // namespace xrpl
