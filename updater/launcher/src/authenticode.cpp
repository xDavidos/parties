#include "authenticode.h"

#include "file_io.h"

#include <format>

#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

namespace parties::update::launcher {

#ifndef PARTIES_AUTHENTICODE_PUBLISHER
#define PARTIES_AUTHENTICODE_PUBLISHER ""
#endif

std::string_view pinned_publisher() { return PARTIES_AUTHENTICODE_PUBLISHER; }

std::optional<std::string> publisher_mismatch(const fs::path& file, std::string_view publisher) {
    const std::wstring path = file.wstring();
    WINTRUST_FILE_INFO info{};
    info.cbStruct = sizeof info;
    info.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{};
    data.cbStruct = sizeof data;
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;

    LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    std::optional<std::string> why;
    if (status != ERROR_SUCCESS) {
        why = std::format("no valid Authenticode signature (0x{:08x})", static_cast<uint32_t>(status));
    } else {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
        CRYPT_PROVIDER_SGNR* signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT* cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        std::wstring name;
        if (cert && cert->pCert) {
            DWORD n = CertGetNameStringW(cert->pCert, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_COMMON_NAME),
                                         nullptr, 0);
            name.resize(n);
            CertGetNameStringW(cert->pCert, CERT_NAME_ATTR_TYPE, 0, const_cast<char*>(szOID_COMMON_NAME), name.data(),
                               n);
            if (!name.empty() && name.back() == L'\0') name.pop_back();
        }
        std::string signer_name = utf8(fs::path(name));
        if (signer_name.empty()) why = "the signer has no common name";
        else if (signer_name != publisher)
            why = "signed by \"" + signer_name + "\", not \"" + std::string(publisher) + "\"";
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return why;
}

} // namespace parties::update::launcher
