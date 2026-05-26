#include "fh6/fmod/pe_image.hpp"

#include <windows.h>
#include <algorithm>
#include <cstring>

namespace fh6::fmod_bridge {

bool PESection::readable() const noexcept {
    return (characteristics & 0x40000000u /* IMAGE_SCN_MEM_READ */) != 0;
}

PEImage parse(std::byte* base) {
    PEImage img;
    if (!base) return img;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return img;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return img;

    img.base = base;
    img.size = nt->OptionalHeader.SizeOfImage;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    img.sections.reserve(nt->FileHeader.NumberOfSections);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        PESection ps;
        std::memcpy(ps.name.data(), sec->Name, 8);
        ps.start           = base + sec->VirtualAddress;
        ps.end             = ps.start + sec->Misc.VirtualSize;
        ps.characteristics = sec->Characteristics;
        img.sections.push_back(ps);

        const char* n = ps.name.data();
        if (std::strncmp(n, ".text", 8) == 0) {
            img.text     = ps.start;
            img.text_end = ps.end;
        } else if (std::strncmp(n, ".rdata", 8) == 0) {
            img.rdata     = ps.start;
            img.rdata_end = ps.end;
        }
    }
    if (!img.text || !img.rdata) return img;

    // .pdata: array of RUNTIME_FUNCTION. For chained-unwind entries
    // (UNW_FLAG_CHAININFO), follow the chain up to the owning function so
    // prologue checks always land on a real entry point.
    auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dd.VirtualAddress || dd.Size < sizeof(RUNTIME_FUNCTION)) return img;

    auto* pf                = reinterpret_cast<RUNTIME_FUNCTION*>(base + dd.VirtualAddress);
    const std::size_t count = dd.Size / sizeof(RUNTIME_FUNCTION);
    img.function_rvas.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        RUNTIME_FUNCTION rf = pf[i];
        for (int hop = 0; hop < 16; ++hop) {
            if (!rf.UnwindData) break;
            if (rf.UnwindData + 4 > img.size) break;
            auto* unwind = base + rf.UnwindData;
            if ((unwind[0] & std::byte{0x20}) == std::byte{0}) break; // not chained
            const std::size_t codes = (2u * std::to_integer<unsigned>(unwind[2]) + 3u) & ~3u;
            if (rf.UnwindData + codes + 16 > img.size) break;
            std::memcpy(&rf, unwind + 4 + codes, sizeof(RUNTIME_FUNCTION));
        }
        if (rf.BeginAddress) img.function_rvas.push_back(rf.BeginAddress);
    }
    std::ranges::sort(img.function_rvas);
    const auto dupes = std::ranges::unique(img.function_rvas);
    img.function_rvas.erase(dupes.begin(), dupes.end());
    return img;
}

} // namespace fh6::fmod_bridge
