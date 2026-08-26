// nxdn_make_sample.cpp
//
// Synthesize an NXDN48 (6.25 kHz, 2400 baud 4FSK) discriminator capture
// carrying a chosen RAN, source ID, destination/talkgroup, and group
// flag, so the NXDN metadata path can be tested against a KNOWN-good
// signal without needing an off-air recording.
//
// Output is an S16LE 48 kHz mono discriminator .dis — the same format
// as DSDcc's samples/*.dis and what dsd-fme reads from stdin — so it can
// be fed straight to either decoder, or FM-wrapped into IQ with
// tools/make_test_bluefile.py to drive the WebSocket server.
//
// WHY IT DECODES: every layer is the exact algebraic inverse of DSDcc's
// NXDN decoder (f4exb/dsdcc nxdn.cpp) — LICH, SACCH and dual FACCH1
// framing, the K=5 rate-1/2 convolutional code, CRC6/CRC12, the SACCH
// and FACCH1 interleave/puncture tables, the PN_9_5 (seed 0xE4) symbol
// scrambler, and the 4FSK dibit->level mapping (dibit 0:+1 1:+3 2:-1
// 3:-3). It was verified bit-exact against DSDcc's own compiled
// convolution+CRC primitives, then end to end against BOTH real decoders:
// the DSDcc library (getNXDNDecoder().getSourceId() etc.) and dsd-fme
// (`dsd-fme -fi`) recover the encoded RAN/source/destination/group.
//
// A full acquisition preamble precedes every frame so the decoder
// re-locks each frame; NXDN's built-in redundancy (dual FACCH1 + SACCH)
// carries the metadata through the occasional per-burst CRC slip that
// idealized synthetic timing produces.
//
// RELIABILITY NOTE: the source/destination/group fields ride on the two
// FACCH1 copies and decode reliably for any values. The RAN rides on the
// single SACCH copy per frame; because these symbols are mathematically
// ideal (no pulse-shaping filter, perfect levels), a real decoder's
// zero-crossing symbol-timing recovery can drift during long runs of
// like symbols and drop the SACCH for some FACCH1 payloads. So RAN is
// reliable for "gentle" IDs (e.g. small values) and best-effort for
// others. This is a synthetic-signal artifact only; an off-air capture
// does not have it. If you need a rock-solid RAN in a synthetic sample,
// pick source/destination values that decode cleanly (verify with
// dsd-fme) or drive RAN home via a run of frames.
//
// Build:  g++ -O2 -o nxdn_make_sample tools/nxdn_make_sample.cpp
// Usage:  ./nxdn_make_sample --ran 9 --src 100 --dst 200 --frames 60
//                            --out nxdn48.dis
// Verify: dsd-fme -fi -i - -o null < nxdn48.dis      (or dsdccx -fi -i nxdn48.dis)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <cmath>

// ---- MSB-first bit access (matches DSDcc WRITE_BIT1/READ_BIT1) -------
static const uint8_t MASK[8]={0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x01};
static inline void wbit(uint8_t*p,unsigned i,int b){ if(b)p[i>>3]|=MASK[i&7]; else p[i>>3]&=~MASK[i&7]; }
static inline int  rbit(const uint8_t*p,unsigned i){ return (p[i>>3]&MASK[i&7])?1:0; }

// ---- CRC6 / CRC12 (from DSDcc nxdncrc.cpp createCRC*) ----------------
static uint8_t createCRC6(const uint8_t*in,unsigned len){ uint8_t c=0x3F; for(unsigned i=0;i<len;i++){bool b1=rbit(in,i),b2=(c&0x20)==0x20; c<<=1; if(b1^b2)c^=0x27;} return c&0x3F; }
static void encodeCRC6(uint8_t*in,unsigned len){ uint8_t c[1]={createCRC6(in,len)}; unsigned n=len; for(unsigned i=2;i<8;i++,n++) wbit(in,n,rbit(c,i)); }
static uint16_t createCRC12(const uint8_t*in,unsigned len){ uint16_t c=0x0FFF; for(unsigned i=0;i<len;i++){bool b1=rbit(in,i),b2=(c&0x0800)==0x0800; c<<=1; if(b1^b2)c^=0x080F;} return c&0x0FFF; }
static void encodeCRC12(uint8_t*in,unsigned len){ uint16_t crc=createCRC12(in,len); uint8_t t[2]={uint8_t(crc>>8),uint8_t(crc&0xFF)}; unsigned n=len; for(unsigned i=4;i<16;i++,n++) wbit(in,n,rbit(t,i)); }

// ---- convolutional encoder (inverse of DSDcc CNXDNConvolution) ------
static void convEncode(const uint8_t*in,uint8_t*out,unsigned n){ uint8_t d1=0,d2=0,d3=0,d4=0; unsigned k=0; for(unsigned i=0;i<n;i++){uint8_t d=rbit(in,i)?1:0; uint8_t g1=(d+d3+d4)&1,g2=(d+d1+d2+d4)&1; d4=d3;d3=d2;d2=d1;d1=d; wbit(out,k++,g1); wbit(out,k++,g2);} }

// ---- PN_9_5 scrambler seed 0xE4 (from DSDcc pn.cpp) -----------------
struct PN { uint8_t bit[512]; PN(unsigned seed){ unsigned sr=seed; for(int i=0;i<512;i++){ unsigned b0=sr&1,b4=(sr&0x10)>>4; sr>>=1; sr|=(b4^b0)<<8; bit[i]=b0; } } };

// ---- interleave / puncture tables (from DSDcc nxdn.cpp) -------------
static const int SACCH_IL[60]={0,12,24,36,48,1,13,25,37,49,2,14,26,38,50,3,15,27,39,51,4,16,28,40,52,5,17,29,41,53,6,18,30,42,54,7,19,31,43,55,8,20,32,44,56,9,21,33,45,57,10,22,34,46,58,11,23,35,47,59};
static const int SACCH_PUNC[12]={5,11,17,23,29,35,41,47,53,59,65,71};
static const int FACCH1_IL[144]={0,16,32,48,64,80,96,112,128,1,17,33,49,65,81,97,113,129,2,18,34,50,66,82,98,114,130,3,19,35,51,67,83,99,115,131,4,20,36,52,68,84,100,116,132,5,21,37,53,69,85,101,117,133,6,22,38,54,70,86,102,118,134,7,23,39,55,71,87,103,119,135,8,24,40,56,72,88,104,120,136,9,25,41,57,73,89,105,121,137,10,26,42,58,74,90,106,122,138,11,27,43,59,75,91,107,123,139,12,28,44,60,76,92,108,124,140,13,29,45,61,77,93,109,125,141,14,30,46,62,78,94,110,126,142,15,31,47,63,79,95,111,127,143};
static const int FACCH1_PUNC[48]={1,5,9,13,17,21,25,29,33,37,41,45,49,53,57,61,65,69,73,77,81,85,89,93,97,101,105,109,113,117,121,125,129,133,137,141,145,149,153,157,161,165,169,173,177,181,185,189};

// info bits -> conv encode -> drop puncture positions -> interleave -> dibits
static void encodeChannel(const uint8_t* info, unsigned infoBits,
                          const int* il, unsigned ilLen,
                          const int* punc, unsigned puncLen,
                          std::vector<uint8_t>& out){
    std::vector<uint8_t> coded((infoBits*2+7)/8, 0);
    convEncode(info, coded.data(), infoBits);
    unsigned codedBits=infoBits*2, pi=0, oi=0;
    std::vector<uint8_t> punctured(ilLen, 0);
    for(unsigned j=0;j<codedBits;j++){ if(pi<puncLen&&(int)j==punc[pi]){pi++;continue;} if(oi<ilLen) punctured[oi]=rbit(coded.data(),j); oi++; }
    for(unsigned n=0;n<ilLen/2;n++){ int hi=punctured[il[2*n]],lo=punctured[il[2*n+1]]; out.push_back((hi<<1)|lo); }
}

int main(int argc,char**argv){
    int RAN=1, SRC=1001, DST=2002, frames=60; bool group=true;
    const char* out="nxdn48.dis";
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--ran")&&i+1<argc) RAN=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--src")&&i+1<argc) SRC=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--dst")&&i+1<argc) DST=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--frames")&&i+1<argc) frames=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--individual")) group=false;
        else if(!strcmp(argv[i],"--out")&&i+1<argc) out=argv[++i];
        else { fprintf(stderr,"unknown arg: %s\n",argv[i]); return 2; }
    }

    // SACCH: 26 payload + CRC6 + 4 tail = 36 bits (carries RAN)
    uint8_t sacch[5]={0,0,0,0,0};
    sacch[0]=(uint8_t)((3<<6)|(RAN&0x3F));   // countdown=3 | RAN
    sacch[1]=0x10; sacch[2]=0x00;
    encodeCRC6(sacch,26);

    // FACCH1: VCALL message 80 bits + CRC12 + 4 tail = 96 bits
    uint8_t fac[12]; memset(fac,0,12);
    fac[0]=0x01;                     // message type VCALL
    fac[2]=group?0x00:0x80;          // group if bit7 clear; fullrate bit0=0 (EHR)
    fac[3]=(SRC>>8)&0xFF; fac[4]=SRC&0xFF;
    fac[5]=(DST>>8)&0xFF; fac[6]=DST&0xFF;
    encodeCRC12(fac,80);

    std::vector<uint8_t> sacchDibits, facchDibits;
    encodeChannel(sacch,36,SACCH_IL,60,SACCH_PUNC,12,sacchDibits);   // 30 dibits
    encodeChannel(fac,  96,FACCH1_IL,144,FACCH1_PUNC,48,facchDibits);// 72 dibits

    // LICH: RDCH(2), SACCH(0), StealBoth(0), outbound(1), even parity
    int lb[8]={1,0, 0,0, 0,0, 1, 0};
    lb[7]=(lb[0]+lb[1]+lb[2]+lb[3]+lb[4]+lb[5])&1;

    // one frame body: LICH(8) + SACCH(30) + FACCH1(72) + FACCH1(72) = 182
    std::vector<uint8_t> frame;
    for(int i=0;i<8;i++) frame.push_back((uint8_t)(lb[i]<<1));
    for(uint8_t d: sacchDibits) frame.push_back(d);
    for(uint8_t d: facchDibits) frame.push_back(d);
    for(uint8_t d: facchDibits) frame.push_back(d);

    PN pn(0xe4);
    for(size_t i=0;i<frame.size();i++) if(pn.bit[i]) frame[i]^=2; // scramble LICH+body

    const int FSW[10]={3,1,3,1,3,3,1,1,3,1};
    const int PRE[9] ={3,1,3,1,1,3,3,3,1};
    auto sd=[](int s){ return s==1?1:3; };  // sync symbol +3 -> dibit1, -3 -> dibit3

    // Re-acquire full sync before every frame (dotting + preamble + FSW),
    // then the frame. Keeps the decoder locked frame-to-frame.
    std::vector<uint8_t> dibits;
    for(int f=0;f<frames;f++){
        for(int i=0;i<24;i++) dibits.push_back(i&1?3:1);
        for(int s: PRE) dibits.push_back(sd(s));
        for(int s: FSW) dibits.push_back(sd(s));
        for(uint8_t d: frame) dibits.push_back(d);
    }

    // 4FSK -> discriminator, raised-cosine smoothed for clean zero crossings
    const int SPS=20, UNIT=3000;   // 48000/2400 baud; +/-3 -> +/-9000
    int lvl[4]={ +1*UNIT, +3*UNIT, -1*UNIT, -3*UNIT };
    std::vector<double> train(SPS*10, 0.0);
    for(uint8_t d: dibits) for(int k=0;k<SPS;k++) train.push_back((double)lvl[d]);
    int FL=SPS|1; std::vector<double> h(FL); double hs=0;
    for(int i=0;i<FL;i++){ h[i]=0.5-0.5*std::cos(2*M_PI*(i+1)/(FL+1)); hs+=h[i]; }
    for(double&v:h) v/=hs;
    std::vector<int16_t> pcm(train.size());
    for(size_t n=0;n<train.size();n++){ double a=0; for(int j=0;j<FL;j++){ long idx=(long)n-FL/2+j; if(idx>=0&&idx<(long)train.size()) a+=train[idx]*h[j]; } pcm[n]=(int16_t)std::lround(a); }

    FILE* fo=fopen(out,"wb"); if(!fo){ perror("open out"); return 1; }
    fwrite(pcm.data(),2,pcm.size(),fo); fclose(fo);
    printf("%s: NXDN48, %d frames, %.2f s — RAN=%d src=%d dst=%d %s\n",
           out, frames, pcm.size()/48000.0, RAN, SRC, DST, group?"group":"individual");
    return 0;
}
