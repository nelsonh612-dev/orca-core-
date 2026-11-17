#include "orca/crypto/aes128.hpp"
#include "test_framework.hpp"
#include <array>
#include <string>
namespace {
template<std::size_t N> std::array<orca::u8,N> hx(const char(&s)[N*2+1]){std::array<orca::u8,N> a{};auto d=[](char c)->unsigned{return c>='0'&&c<='9'?static_cast<unsigned>(c-'0'):static_cast<unsigned>((c|32)-'a'+10);};for(std::size_t i=0;i<N;i++)a[i]=static_cast<orca::u8>((d(s[2*i])<<4U)|d(s[2*i+1]));return a;}
}
TEST_CASE(aes128_cbc_nist_sp800_38a){using namespace orca::crypto;auto k=hx<16>("2b7e151628aed2a6abf7158809cf4f3c");auto iv=hx<16>("000102030405060708090a0b0c0d0e0f");auto data=hx<16>("7649abac8119b246cee98e9b12e9197d");auto expected=hx<16>("6bc1bee22e409f96e93d7e117393172a");std::string e;CHECK(aes128_cbc_decrypt(k,iv,data,e));CHECK(data==expected);CHECK(e.empty());}
TEST_CASE(aes128_cbc_rejects_partial_block){orca::crypto::Aes128Key k{};orca::crypto::Aes128Iv iv{};std::array<orca::u8,15>d{};std::string e;CHECK(!orca::crypto::aes128_cbc_decrypt(k,iv,d,e));CHECK(!e.empty());}
TEST_CASE(aes128_cbc_empty_input){orca::crypto::Aes128Key k{};orca::crypto::Aes128Iv iv{};std::span<orca::u8>d;std::string e;CHECK(orca::crypto::aes128_cbc_decrypt(k,iv,d,e));}
TEST_MAIN("crypto")
