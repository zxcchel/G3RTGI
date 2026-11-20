#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>

struct ShaderConstant {
    std::string name;
    uint16_t regSet;   // 0=BOOL, 1=INT4, 2=FLOAT4, 3=SAMPLER
    uint16_t regIndex;
    uint16_t regCount;
};

struct ShaderInfo {
    uint64_t hash = 0;
    bool isPixel = false;
    std::string target;
    std::vector<ShaderConstant> constants;
};

inline uint64_t HashShaderBytecode(const uint32_t* pFunc) {
    if (!pFunc) return 0;
    uint64_t h = 14695981039346656037ULL;
    const uint32_t* p = pFunc;
    for (int i = 0; i < 65536; ++i) {
        uint32_t tok = p[i];
        h ^= tok;
        h *= 1099511628211ULL;
        if (tok == 0x0000FFFF) break; // D3DSIO_END
    }
    return h;
}

inline ShaderInfo ParseShaderCTAB(const uint32_t* pFunc, bool isPixel) {
    ShaderInfo info;
    info.isPixel = isPixel;
    info.hash = HashShaderBytecode(pFunc);
    if (!pFunc) return info;

    // Пропускаем version token (pFunc[0])
    const uint32_t* p = pFunc + 1;
    while (*p != 0x0000FFFF) {
        uint32_t tok = *p;
        if ((tok & 0xFFFF) == 0xFFFE) { // D3DSIO_COMMENT
            uint32_t dwordCount = (tok >> 16) & 0x7FFF;
            if (dwordCount > 7 && p[1] == 0x42415443) { // 'CTAB'
                const uint8_t* base = reinterpret_cast<const uint8_t*>(&p[2]);
                struct CTABHeader {
                    uint32_t Size;
                    uint32_t Creator;
                    uint32_t Version;
                    uint32_t Constants;
                    uint32_t ConstantInfo;
                    uint32_t Flags;
                    uint32_t Target;
                };
                struct CTABConstInfo {
                    uint32_t Name;
                    uint16_t RegisterSet;
                    uint16_t RegisterIndex;
                    uint16_t RegisterCount;
                    uint16_t Reserved;
                    uint32_t TypeInfo;
                    uint32_t DefaultValue;
                };
                const auto* hdr = reinterpret_cast<const CTABHeader*>(base);
                if (hdr->Target) {
                    info.target = reinterpret_cast<const char*>(base + hdr->Target);
                }
                const auto* cinfo = reinterpret_cast<const CTABConstInfo*>(base + hdr->ConstantInfo);
                for (uint32_t i = 0; i < hdr->Constants; ++i) {
                    ShaderConstant sc;
                    sc.name = reinterpret_cast<const char*>(base + cinfo[i].Name);
                    sc.regSet = cinfo[i].RegisterSet;
                    sc.regIndex = cinfo[i].RegisterIndex;
                    sc.regCount = cinfo[i].RegisterCount;
                    info.constants.push_back(std::move(sc));
                }
                break;
            }
            p += 1 + dwordCount;
        }
        else {
            // Обычная инструкция: для SM2/3 длина может быть закодирована в битах 24..27
            uint32_t len = (tok >> 24) & 0x0F;
            p += 1 + len;
        }
    }
    return info;
}

inline const char* RegSetName(uint16_t rs) {
    switch (rs) {
    case 0: return "b";
    case 1: return "i";
    case 2: return "c";
    case 3: return "s";
    default: return "?";
    }
}