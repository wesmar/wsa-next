// WSA-Next © 2026 Marek Wesołowski - wesmar
#include "support/Signature.h"
#include "support/Common.h"
#include <Windows.h>
#include <SoftPub.h>
#include <WinTrust.h>
#include "support/Sha256.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace WN::Signature {
namespace {

// The publisher's code signing certificate (Certum Open Source Code Signing). The subject stays
// the same when the certificate is renewed; the chain must lead to Certum's trusted network root
// ("Certum Trusted Network CA 2", which Windows may continue through its cross-signed
// predecessor "Certum Trusted Network CA").
constexpr wchar_t kPublisher[] = L"Open Source Developer Marek Wesołowski";
constexpr std::wstring_view kRootPrefix = L"Certum Trusted Network CA";
// The roots themselves, by the SHA-256 of their public key (SubjectPublicKeyInfo): a name alone
// would also match a look-alike root someone added to the certificate store.
constexpr std::wstring_view kRootKeys[] = {
    L"6b3b57e9ec88d1bb3d01637ff33c7698b3c9758255e9f01ea9178f3e7f3b2b52",  // Certum Trusted Network CA 2
    L"aa2630a7b617b04d0a294bab7a8caaa5016e6dbe604837a83a85719fab667eb5",  // Certum Trusted Network CA
};

std::wstring KeyDigest(PCCERT_CONTEXT cert) {
    DWORD size = 0;
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &cert->pCertInfo->SubjectPublicKeyInfo, 0, nullptr, nullptr, &size))
        return {};
    std::vector<BYTE> der(size);
    if (!CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, &cert->pCertInfo->SubjectPublicKeyInfo, 0, nullptr, der.data(), &size))
        return {};
    Sha256 sha;
    if (!sha.ok() || !sha.Update(der.data(), size)) return {};
    return sha.Finish();
}

std::wstring NameOf(PCCERT_CONTEXT cert, DWORD flags = 0) {
    const DWORD size = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, nullptr, 0);
    std::wstring name(size, L'\0');
    CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, flags, nullptr, name.data(), size);
    if (!name.empty() && name.back() == L'\0') name.pop_back();
    return name;
}

bool TrustedRoot(PCCERT_CONTEXT root) {
    if (!NameOf(root).starts_with(kRootPrefix)) return false;
    const std::wstring key = KeyDigest(root);
    return std::ranges::find(kRootKeys, std::wstring_view(key)) != std::end(kRootKeys);
}

}  // namespace

Origin CheckFile(const wchar_t* path, void* handle) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof file;
    file.pcwszFilePath = path;
    file.hFile = static_cast<HANDLE>(handle);  // the bytes the caller holds open, when given
    WINTRUST_DATA data{};
    data.cbStruct = sizeof data;
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;  // offline: no network access at every start
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);

    Origin origin = Origin::Invalid;
    if (status == TRUST_E_NOSIGNATURE || status == TRUST_E_SUBJECT_FORM_UNKNOWN || status == TRUST_E_PROVIDER_UNKNOWN) {
        origin = Origin::Unsigned;
    } else if (status == ERROR_SUCCESS) {
        origin = Origin::OtherSigner;
        const CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        const CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(const_cast<CRYPT_PROVIDER_DATA*>(provider), 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain > 1 && NameOf(signer->pasCertChain[0].pCert) == kPublisher &&
            TrustedRoot(signer->pasCertChain[signer->csCertChain - 1].pCert))
            origin = Origin::Official;
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return origin;
}

Origin Check() {
    static const Origin origin = [] {
        const std::wstring path = ModulePath();
        return path.empty() ? Origin::Invalid : CheckFile(path.c_str());
    }();
    return origin;
}

}  // namespace WN::Signature
