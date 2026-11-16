#include "orca/crypto/aes128.hpp"
#include <array>
#include <cstddef>

namespace orca::crypto {
namespace {
using Block = std::array<u8,16>;
u8 xtime(u8 x) noexcept { return static_cast<u8>((x << 1U) ^ ((x & 0x80U) ? 0x1BU : 0U)); }
u8 mul(u8 a,u8 b) noexcept { u8 r=0; for(int i=0;i<8;++i){ if(b&1U) r^=a; a=xtime(a); b>>=1U; } return r; }
u8 rotl8(u8 x,unsigned n) noexcept { return static_cast<u8>((x<<n)|(x>>(8U-n))); }
u8 gf_pow(u8 a,unsigned n) noexcept { u8 r=1; while(n){ if(n&1U) r=mul(r,a); a=mul(a,a); n>>=1U; } return r; }
u8 sbox(u8 x) noexcept { const u8 y=x?gf_pow(x,254U):0U; return static_cast<u8>(y^rotl8(y,1)^rotl8(y,2)^rotl8(y,3)^rotl8(y,4)^0x63U); }
const std::array<u8,256>& inv_sbox() noexcept {
 static const auto t=[] { std::array<u8,256> a{}; for(unsigned i=0;i<256;++i) a[sbox(static_cast<u8>(i))]=static_cast<u8>(i); return a; }();
 return t;
}
using RoundKeys=std::array<u8,176>;
RoundKeys expand(const Aes128Key& key) noexcept {
 RoundKeys w{}; for(unsigned i=0;i<16;++i) w[i]=key[i];
 u8 rcon=1; unsigned n=16;
 while(n<176){ u8 t[4]={w[n-4],w[n-3],w[n-2],w[n-1]};
  if(n%16==0){ const u8 q=t[0]; t[0]=sbox(t[1]);t[1]=sbox(t[2]);t[2]=sbox(t[3]);t[3]=sbox(q);t[0]^=rcon;rcon=xtime(rcon); }
  for(unsigned i=0;i<4;++i){w[n]=static_cast<u8>(w[n-16]^t[i]);++n;}
 }
 return w;
}
void add_key(Block& s,const RoundKeys& k,unsigned round) noexcept { for(unsigned i=0;i<16;++i)s[i]^=k[round*16+i]; }
void inv_shift(Block& s) noexcept { Block t=s; for(unsigned r=0;r<4;++r)for(unsigned c=0;c<4;++c)s[r+4*c]=t[r+4*((c+4-r)%4)]; }
void inv_sub(Block& s) noexcept { const auto& t=inv_sbox(); for(auto& x:s)x=t[x]; }
void inv_mix(Block& s) noexcept {
 for(unsigned c=0;c<4;++c){ const unsigned i=4*c; const u8 a=s[i],b=s[i+1],d=s[i+2],e=s[i+3];
  s[i]=static_cast<u8>(mul(a,14)^mul(b,11)^mul(d,13)^mul(e,9));
  s[i+1]=static_cast<u8>(mul(a,9)^mul(b,14)^mul(d,11)^mul(e,13));
  s[i+2]=static_cast<u8>(mul(a,13)^mul(b,9)^mul(d,14)^mul(e,11));
  s[i+3]=static_cast<u8>(mul(a,11)^mul(b,13)^mul(d,9)^mul(e,14));
 }
}
Block decrypt_block(const Block& in,const RoundKeys& k) noexcept {
 Block s=in; add_key(s,k,10);
 for(unsigned round=9;round>0;--round){inv_shift(s);inv_sub(s);add_key(s,k,round);inv_mix(s);}
 inv_shift(s);inv_sub(s);add_key(s,k,0); return s;
}
} // namespace
bool aes128_cbc_decrypt(const Aes128Key& key,Aes128Iv& iv,std::span<u8> data) noexcept {
 if(data.size()%16U) return false;
 const RoundKeys keys=expand(key);
 for(std::size_t off=0;off<data.size();off+=16){
  Block cipher{}; for(unsigned i=0;i<16;++i)cipher[i]=data[off+i];
  Block plain=decrypt_block(cipher,keys);
  for(unsigned i=0;i<16;++i)data[off+i]=static_cast<u8>(plain[i]^iv[i]);
  iv=cipher;
 }
 return true;
}
} // namespace orca::crypto
