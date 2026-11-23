// WSA-Next © 2026 Marek Wesołowski - wesmar
// Windows Hello user verification through WinRT UserConsentVerifier (fingerprint, face or PIN).
#pragma once
#include <stop_token>
#include <string>
#include <string_view>

namespace WN::Bio {

enum class HelloStatus {
    Available,
    DeviceNotPresent,
    NotConfigured,
    DisabledByPolicy,
    DeviceBusy,
    Error,
};

enum class VerifyResult {
    Verified,
    Canceled,
    NotAvailable,
    Busy,
    Failed,
};

// Queries whether Windows Hello is configured for the current interactive Windows user.
HelloStatus CheckAvailability();

// Short human-readable description of a HelloStatus (CLI diagnostics and logs).
std::wstring StatusText(HelloStatus status);

// Prompts the current Windows user with the native Windows Hello dialog (fingerprint / PIN / face)
// and waits for the answer; a stop request on `cancel` closes the dialog (result Canceled).
VerifyResult RequestVerification(std::wstring_view message, std::stop_token cancel = {});

}  // namespace WN::Bio
