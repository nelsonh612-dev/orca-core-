#include "orca/crypto/sha1.hpp"
#include <array>
#include <vector>

namespace orca::crypto {
namespace {
constexpr u32 rol(u32 x,unsigned n) noexcept { return (x<<n)|(x>>(32U-n)); }
}
Sha1Digest sha1(std::span<const u8> data) noexcept {
    u32 h0=0x67452301U,h1=0xEFCDAB89U,h2=0x98BADCFEU,h3=0x10325476U,h4=0xC3D2E1F0U;
    const u64 bit_len=static_cast<u64>(data.size())*8ULL;
    const std::size_t total=((data.size()+9U+63U)/64U)*64U;
    std::vector<u8> msg(total,0);
    for(std::size_t i=0;i<data.size();++i) msg[i]=data[i];
    msg[data.size()]=0x80U;
    for(unsigned i=0;i<8;++i) msg[total-1U-i]=static_cast<u8>(bit_len>>(i*8U));
    for(std::size_t off=0;off<total;off+=64U) {
        std::array<u32,80> w{};
        for(unsigned i=0;i<16;++i) {
            const std::size_t p=off+i*4U;
            w[i]=(u32{msg[p]}<<24U)|(u32{msg[p+1]}<<16U)|(u32{msg[p+2]}<<8U)|u32{msg[p+3]};
        }
        for(unsigned i=16;i<80;++i) w[i]=rol(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
        u32 a=h0,b=h1,c=h2,d=h3,e=h4;
        for(unsigned i=0;i<80;++i) {
            u32 f=0,k=0;
            if(i<20){f=(b&c)|((~b)&d);k=0x5A827999U;}
            else if(i<40){f=b^c^d;k=0x6ED9EBA1U;}
            else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDCU;}
            else {f=b^c^d;k=0xCA62C1D6U;}
            const u32 temp=rol(a,5)+f+e+k+w[i]; e=d;d=c;c=rol(b,30);b=a;a=temp;
        }
        h0+=a;h1+=b;h2+=c;h3+=d;h4+=e;
    }
    Sha1Digest out{};
    const u32 hs[5]={h0,h1,h2,h3,h4};
    for(unsigned i=0;i<5;++i) for(unsigned j=0;j<4;++j) out[i*4U+j]=static_cast<u8>(hs[i]>>(24U-j*8U));
    return out;
}
} // namespace orca::crypto
