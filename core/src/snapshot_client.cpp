#include "snapshot_client.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "kmp_log.h"

namespace kmp {

namespace {

static std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                static_cast<int>(s.size()), NULL, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        &w[0], n);
    return w;
}

} // namespace

SnapshotDownloadResult download_snapshot_blocking(
    const std::string& host,
    uint16_t           port,
    const std::string& out_path,
    const SnapshotProgressCb& cb,
    volatile long*     cancel_flag,
    int*               http_status_out) {

    if (http_status_out) *http_status_out = 0;

    HINTERNET hSession = WinHttpOpen(L"KenshiMP/1.0",
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return SNAPSHOT_DOWNLOAD_CONNECT_FAILED;

    std::wstring whost = utf8_to_wide(host);
    HINTERNET hConn = WinHttpConnect(hSession, whost.c_str(),
                                     static_cast<INTERNET_PORT>(port), 0);
    if (!hConn) {
        WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_CONNECT_FAILED;
    }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", L"/snapshot",
                                        NULL, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq) {
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_CONNECT_FAILED;
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_CONNECT_FAILED;
    }

    if (!WinHttpReceiveResponse(hReq, NULL)) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_CONNECT_FAILED;
    }

    // Status code.
    DWORD status = 0;
    DWORD status_sz = sizeof(status);
    WinHttpQueryHeaders(hReq,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_sz,
        WINHTTP_NO_HEADER_INDEX);
    if (http_status_out) *http_status_out = static_cast<int>(status);
    if (status < 200 || status >= 300) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_HTTP_ERROR;
    }

    // Content-Length (may be missing).
    uint64_t total = 0;
    {
        DWORD cl = 0;
        DWORD cl_sz = sizeof(cl);
        if (WinHttpQueryHeaders(hReq,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &cl, &cl_sz,
                WINHTTP_NO_HEADER_INDEX)) {
            total = static_cast<uint64_t>(cl);
        }
    }

    FILE* out = NULL;
    if (fopen_s(&out, out_path.c_str(), "wb") != 0 || !out) {
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return SNAPSHOT_DOWNLOAD_WRITE_FAILED;
    }

    uint64_t done = 0;
    const DWORD CHUNK = 32 * 1024;
    std::vector<BYTE> buf(CHUNK);
    SnapshotDownloadResult rc = SNAPSHOT_DOWNLOAD_OK;

    while (true) {
        if (cancel_flag && InterlockedCompareExchange(cancel_flag, 0, 0) != 0) {
            rc = SNAPSHOT_DOWNLOAD_CANCELLED;
            break;
        }
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hReq, &avail)) {
            rc = SNAPSHOT_DOWNLOAD_UNKNOWN;
            break;
        }
        if (avail == 0) break;  // complete

        DWORD read = 0;
        DWORD want = (avail > CHUNK) ? CHUNK : avail;
        if (!WinHttpReadData(hReq, buf.data(), want, &read) || read == 0) {
            rc = SNAPSHOT_DOWNLOAD_UNKNOWN;
            break;
        }
        if (std::fwrite(buf.data(), 1, read, out) != read) {
            rc = SNAPSHOT_DOWNLOAD_WRITE_FAILED;
            break;
        }
        done += read;
        if (cb) cb(done, total);
    }

    std::fclose(out);
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);

    if (rc != SNAPSHOT_DOWNLOAD_OK) {
        DeleteFileA(out_path.c_str());
    }
    return rc;
}

} // namespace kmp
