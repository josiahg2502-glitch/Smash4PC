#include "guest_format.h"

#include "cafe/guest.h"
#include "cafe/ppc_ops.h"

#include <cstdio>

namespace cafe::os {
namespace {

struct VarArgs {
    PPCContext& ctx;
    int gpr;
    int fpr = 1;
    uint32_t overflow; // next stack argument

    uint32_t next32() {
        if (gpr <= 10) return ctx.r[gpr++];
        const uint32_t v = ppc::ld32(guest_base(), overflow);
        overflow += 4;
        return v;
    }
    uint64_t next64() {
        if (gpr <= 9) {
            if ((gpr & 1) == 0) ++gpr; // pairs start in odd registers
            const uint64_t v = (uint64_t{ctx.r[gpr]} << 32) | ctx.r[gpr + 1];
            gpr += 2;
            return v;
        }
        gpr = 11;
        overflow = (overflow + 7) & ~7u;
        const uint64_t v = ppc::ld64(guest_base(), overflow);
        overflow += 8;
        return v;
    }
    double next_double() {
        if (fpr <= 8) return ctx.f[fpr++].ps0;
        overflow = (overflow + 7) & ~7u;
        const double v = ppc::from_bits(ppc::ld64(guest_base(), overflow));
        overflow += 8;
        return v;
    }
};

} // namespace

std::string format_guest(PPCContext& ctx, uint32_t format_address, int first_gpr) {
    // Stack arguments follow the 8-byte frame header of the caller's frame.
    VarArgs args{ctx, first_gpr, 1, ctx.r[1] + 8};
    const std::string_view format = guest_string(format_address);
    std::string out;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '%') {
            out += format[i];
            continue;
        }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < format.size() && std::string_view("-+ #0").find(format[j]) != std::string_view::npos) {
            spec += format[j++];
        }
        const auto number = [&] {
            if (j < format.size() && format[j] == '*') {
                spec += std::to_string(static_cast<int32_t>(args.next32()));
                ++j;
                return;
            }
            while (j < format.size() && format[j] >= '0' && format[j] <= '9') spec += format[j++];
        };
        number();
        if (j < format.size() && format[j] == '.') {
            spec += format[j++];
            number();
        }
        int longs = 0;
        while (j < format.size() && std::string_view("hlLqjzt").find(format[j]) != std::string_view::npos) {
            if (format[j] == 'l' || format[j] == 'L' || format[j] == 'q' || format[j] == 'j') ++longs;
            if (format[j] == 'q' || format[j] == 'j') ++longs;
            ++j;
        }
        if (j >= format.size()) break;
        const char conversion = format[j];
        char buffer[512];
        switch (conversion) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (longs >= 2) {
                spec += "ll";
                spec += conversion;
                std::snprintf(buffer, sizeof buffer, spec.c_str(),
                              static_cast<unsigned long long>(args.next64()));
            } else {
                spec += conversion;
                const uint32_t v = args.next32();
                if (conversion == 'd' || conversion == 'i') {
                    std::snprintf(buffer, sizeof buffer, spec.c_str(), static_cast<int32_t>(v));
                } else {
                    std::snprintf(buffer, sizeof buffer, spec.c_str(), v);
                }
            }
            out += buffer;
            break;
        }
        case 'p':
            std::snprintf(buffer, sizeof buffer, "0x%08X", args.next32());
            out += buffer;
            break;
        case 's': {
            spec += 's';
            const uint32_t address = args.next32();
            const std::string text = address ? std::string(guest_string(address)) : "(null)";
            std::snprintf(buffer, sizeof buffer, spec.c_str(), text.c_str());
            out += buffer;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
            spec += conversion;
            std::snprintf(buffer, sizeof buffer, spec.c_str(), args.next_double());
            out += buffer;
            break;
        case '%':
            out += '%';
            break;
        default:
            out += spec;
            out += conversion;
            break;
        }
        i = j;
    }
    return out;
}

} // namespace cafe::os
