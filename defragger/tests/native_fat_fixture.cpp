// SPDX-License-Identifier: GPL-3.0-or-later
// Defragmenter — independent FAT12/16/32 test-media generator.
#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using Bytes = std::vector<std::uint8_t>;
constexpr std::uint64_t bps = 512;

// Write explicit little-endian fields; host byte order never affects the fixture.
static void put(Bytes& b, std::size_t off, std::uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; ++i) b.at(off+i) = static_cast<std::uint8_t>(v >> (8*i));
}
static void literal(Bytes& b, std::size_t off, const std::string& s) {
    for (char c : s) b.at(off++) = static_cast<std::uint8_t>(c);
}
static void write(std::ofstream& f, std::uint64_t off, const Bytes& b) {
    f.seekp(static_cast<std::streamoff>(off));
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    if (!f) throw std::runtime_error("fixture write failed");
}
static void generate(const std::string& kind, const std::string& path, const std::string& option) {
    const bool wide = kind == "fat32", small = kind == "fat12";
    if (!wide && !small && kind != "fat16") throw std::runtime_error("unknown FAT width");
    std::uint64_t total = small ? 2880 : 64000;
    if (wide) {
        std::size_t pos = 0;
        total = std::stoull(option, &pos);
        if (pos != option.size() || option.empty() || option.front() == '-' || total < 65525 ||
            total > 0x0fffffffULL) throw std::runtime_error("invalid FAT32 sector count");
    }
    const std::uint64_t reserved = wide ? 32 : 1, roots = wide ? 0 : (small ? 224 : 512);
    std::uint64_t spf = wide ? 1 : (small ? 9 : 250);
    // Monotone sizing avoids the one-sector oscillation of fixed-point FAT sizing.
    if (wide) for (;;) {
        if (total <= reserved + 2*spf) throw std::runtime_error("volume too small");
        const auto needed = ((total-reserved-2*spf+2)*4+bps-1)/bps;
        if (needed <= spf) break;
        spf = needed;
    }
    const auto data = reserved + 2*spf + (roots*32+bps-1)/bps;
    const auto clusters = total-data;
    if (wide && clusters < 65525) throw std::runtime_error("not enough FAT32 clusters");
    const auto media = small ? 0xf0U : 0xf8U;
    Bytes boot(bps), fat(spf*bps), root(bps), info(bps);
    boot[0]=0xeb; boot[1]=wide?0x58:0x3c; boot[2]=0x90;
    literal(boot,3,wide?"MSWIN4.1":"MSDOS5.0");
    put(boot,11,bps,2); boot[13]=1; put(boot,14,reserved,2); boot[16]=2;
    put(boot,17,roots,2); boot[21]=media;
    put(boot,24,wide?63:18,2); put(boot,26,wide?255:2,2);
    boot[510]=0x55; boot[511]=0xaa;
    if (wide) {
        put(boot,32,total,4); put(boot,36,spf,4); put(boot,44,2,4);
        put(boot,48,1,2); put(boot,50,6,2); boot[64]=0x80; boot[66]=0x29;
        put(boot,67,0x1234abcd,4); literal(boot,71,"DEFRAGTEST "); literal(boot,82,"FAT32   ");
        put(info,0,0x41615252,4); put(info,484,0x61417272,4);
        put(info,488,0xffffffff,4); put(info,492,3,4); put(info,508,0xaa550000,4);
    } else {
        put(boot,19,total,2); put(boot,22,spf,2); boot[36]=0x80; boot[38]=0x29;
        put(boot,39,0x1234abcd,4); literal(boot,43,"FRAGTEST   ");
        literal(boot,54,small?"FAT12   ":"FAT16   ");
    }
    const auto eoc = wide?0x0fffffffU:(small?0xfffU:0xffffU);
    const auto setfat = [&](unsigned c, unsigned v) {
        if (!small) { put(fat,c*(wide?4:2),v,wide?4:2); return; }
        // FAT12 entries share a byte. Preserve the neighbouring entry's nibble.
        const auto off=c+c/2; unsigned pair=fat.at(off)|(unsigned(fat.at(off+1))<<8);
        pair=(c&1)?((pair&0x000f)|((v&0xfff)<<4)):((pair&0xf000)|(v&0xfff));
        put(fat,off,pair,2);
    };
    setfat(0,wide?0x0ffffff8U:(small?(0xff0U|media):(0xfff8U|media)));
    setfat(1,eoc); if (wide) setfat(2,eoc);
    const std::array<unsigned,3> chain = !wide && option=="gapped" ?
        std::array<unsigned,3>{10,11,12} : std::array<unsigned,3>{5,10,6};
    setfat(chain[0],chain[1]); setfat(chain[1],chain[2]); setfat(chain[2],eoc);
    literal(root,0,"FILE    BIN"); root[11]=0x20;
    put(root,26,chain[0],2); put(root,28,3*bps,4);
    // Sparse allocation keeps the large-map regression inexpensive without altering bytes.
    std::ofstream f(path,std::ios::binary|std::ios::trunc);
    if (!f) throw std::runtime_error("cannot open fixture output");
    write(f,total*bps-1,Bytes(1)); write(f,0,boot);
    if (wide) { write(f,bps,info); write(f,6*bps,boot); write(f,7*bps,info); }
    for (unsigned i=0;i<2;++i) write(f,(reserved+i*spf)*bps,fat);
    write(f,(wide?data:reserved+2*spf)*bps,root);
    for (unsigned i=0;i<chain.size();++i) write(f,(data+chain[i]-2)*bps,Bytes(bps,static_cast<std::uint8_t>('A'+i)));
    f.close(); if (!f) throw std::runtime_error("fixture close failed");
    std::cout << kind << ' ' << clusters << ' ' << data << '\n';
}
int main(int argc,char** argv) {
    try {
        if (argc<3 || argc>4) throw std::runtime_error("usage: native-fat-fixture fat12|fat16|fat32 OUTPUT [MODE|SECTORS]");
        const std::string kind=argv[1];
        generate(kind,argv[2],argc==4?argv[3]:(kind=="fat32"?"131072":"fragmented"));
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
