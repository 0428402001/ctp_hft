// SPDX-License-Identifier: MIT
// ctp_compat.h - small shims between the SFIT CTP Mini SDK and the standard
// CTP (6.x) SDK, which share class and struct names but differ in a few
// method signatures. Build with -DHFT_CTP_STD=1 to compile against the
// standard SDK headers (sdk/stdctp, for SimNow / ordinary CTP brokers);
// leave it undefined for the CTP Mini flavour (../win64, CTPII Mini fronts).
//
// Deliberately includes no SDK header: the gateway translation units already
// include the right API headers for their process, and the template below is
// only instantiated on the concrete (complete) type at the call site.
#ifndef HFT_CTP_COMPAT_H
#define HFT_CTP_COMPAT_H

namespace hft {
namespace ctp {

#if defined(HFT_CTP_STD)

// Standard CTP: Init() takes no argument.
template <typename CtpApi>
inline void apiInit(CtpApi* api) {
    api->Init();
}

#else

// CTP Mini: Init(bool bResumeRecovery).
template <typename CtpApi>
inline void apiInit(CtpApi* api) {
    api->Init(false);
}

#endif

inline const char* flavourName() {
#if defined(HFT_CTP_STD)
    return "standard CTP";
#else
    return "CTP Mini";
#endif
}

} // namespace ctp
} // namespace hft

#endif // HFT_CTP_COMPAT_H
