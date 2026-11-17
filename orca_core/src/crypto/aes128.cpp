#include "orca/crypto/aes128.hpp"
#include <algorithm>
namespace orca::crypto { namespace {
using B=std::array<u8,16>; using Keys=std::array<B,11>;
u8 xt(u8 x){return static_cast<u8>((x<<1U)^((x&0x80U)?0x1BU:0U));}
u8 mul(u8 a,u8 b){u8 r=0;for(unsigned i=0;i<8;i++){if(b&1U)r^=a;a=xt(a);b=static_cast<u8>(b>>1U);}return r;}
u8 rot(u8 x,unsigned n){return static_cast<u8>((static_cast<unsigned>(x)<<n)|(static_cast<unsigned>(x)>>(8U-n)));}
u8 pw(u8 x,unsigned n){u8 r=1;while(n){if(n&1U)r=mul(r,x);x=mul(x,x);n>>=1U;}return r;}
u8 sb(u8 x){u8 v=x?pw(x,254):0;return static_cast<u8>(v^rot(v,1)^rot(v,2)^rot(v,3)^rot(v,4)^0x63U);}
const std::array<u8,256>& isb(){static const auto t=[](){std::array<u8,256>a{};for(unsigned i=0;i<256;i++)a[sb(static_cast<u8>(i))]=static_cast<u8>(i);return a;}();return t;}
Keys expand(const Aes128Key& k){Keys a{};a[0]=k;u8 rc=1;for(unsigned n=1;n<=10;n++){auto& p=a[n-1];auto& q=a[n];std::array<u8,4> t{sb(p[13]),sb(p[14]),sb(p[15]),sb(p[12])};t[0]^=rc;rc=xt(rc);for(unsigned r=0;r<4;r++)q[r]=static_cast<u8>(p[r]^t[r]);for(unsigned c=1;c<4;c++)for(unsigned r=0;r<4;r++)q[c*4+r]=static_cast<u8>(p[c*4+r]^q[(c-1)*4+r]);}return a;}
void add(B& s,const B& k){for(unsigned i=0;i<16;i++)s[i]^=k[i];}
void shift(B& s){B o=s;for(unsigned r=0;r<4;r++)for(unsigned c=0;c<4;c++)s[c*4+r]=o[((c+4-r)%4)*4+r];}
void sub(B& s){const auto& t=isb();for(auto& x:s)x=t[x];}
void mix(B& s){for(unsigned c=0;c<4;c++){unsigned i=c*4;u8 a=s[i],b=s[i+1],d=s[i+2],e=s[i+3];s[i]=static_cast<u8>(mul(a,14)^mul(b,11)^mul(d,13)^mul(e,9));s[i+1]=static_cast<u8>(mul(a,9)^mul(b,14)^mul(d,11)^mul(e,13));s[i+2]=static_cast<u8>(mul(a,13)^mul(b,9)^mul(d,14)^mul(e,11));s[i+3]=static_cast<u8>(mul(a,11)^mul(b,13)^mul(d,9)^mul(e,14));}}
B dec(const B& in,const Keys& k){B s=in;add(s,k[10]);for(unsigned n=9;n>0;n--){shift(s);sub(s);add(s,k[n]);mix(s);}shift(s);sub(s);add(s,k[0]);return s;}
}
bool aes128_cbc_decrypt(const Aes128Key& key,const Aes128Iv& iv,std::span<u8> data,std::string& error){
if(data.size()%16U){error="AES-128-CBC input length is not a multiple of 16 bytes";return false;}
const Keys keys=expand(key);B prev=iv;
for(std::size_t off=0;off<data.size();off+=16){B cipher{};std::copy_n(data.begin()+static_cast<std::ptrdiff_t>(off),16,cipher.begin());B plain=dec(cipher,keys);for(unsigned i=0;i<16;i++)data[off+i]=static_cast<u8>(plain[i]^prev[i]);prev=cipher;}
error.clear();return true;
}
}
