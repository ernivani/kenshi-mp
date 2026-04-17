#pragma once

// kmp_log.h — KenshiMP logging to separate per-instance file
//
// Each Kenshi instance claims the lowest free index N starting at 1,
// writing to mods/KenshiMP/kenshimp_N.log. The file is held open with
// an exclusive write lock for the process lifetime: a second instance
// finds index 1 locked, falls through to 2, etc. When an instance
// exits the OS releases the handle and the index is reusable.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <string>
#include <sstream>
#include <OgreLogManager.h>

namespace kmp {

class KmpLog {
public:
    static KmpLog& get() {
        static KmpLog instance;
        return instance;
    }

    void init() {
        if (m_handle != INVALID_HANDLE_VALUE) return;

        // Try indices 1..32 — open with no FILE_SHARE_WRITE so a second
        // instance fails with ERROR_SHARING_VIOLATION and moves on.
        DWORD pid = GetCurrentProcessId();
        m_index = 0;
        for (int i = 1; i <= 32; ++i) {
            std::ostringstream ss;
            ss << "mods\\KenshiMP\\kenshimp_" << i << ".log";
            std::string path = ss.str();

            HANDLE h = CreateFileA(
                path.c_str(),
                GENERIC_WRITE,
                FILE_SHARE_READ,        // allow tail/cat, not concurrent write
                NULL,
                CREATE_ALWAYS,          // truncate if exists, create otherwise
                FILE_ATTRIBUTE_NORMAL,
                NULL);
            if (h != INVALID_HANDLE_VALUE) {
                m_handle = h;
                m_index = i;
                m_filename = path;
                break;
            }
            // ERROR_SHARING_VIOLATION (32) means another instance holds it
            DWORD err = GetLastError();
            if (err != ERROR_SHARING_VIOLATION) {
                // Some other error (permissions, disk full…) — keep trying
                // but log the error type once.
                continue;
            }
        }

        if (m_handle != INVALID_HANDLE_VALUE) {
            char buf[128];
            int n = _snprintf(buf, sizeof(buf),
                "=== KenshiMP Log (slot %d, PID %lu) ===\r\n", m_index, pid);
            DWORD wrote = 0;
            if (n > 0) WriteFile(m_handle, buf, (DWORD)n, &wrote, NULL);
        }
    }

    void log(const std::string& msg) {
        if (m_handle != INVALID_HANDLE_VALUE) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            char buf[1024];
            int n = _snprintf(buf, sizeof(buf), "%02d:%02d:%02d: %s\r\n",
                st.wHour, st.wMinute, st.wSecond, msg.c_str());
            if (n > 0) {
                DWORD wrote = 0;
                WriteFile(m_handle, buf, (DWORD)n, &wrote, NULL);
                FlushFileBuffers(m_handle);
            }
        }

        // Also write to Ogre log
        Ogre::LogManager::getSingleton().logMessage(msg);
    }

    void shutdown() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
    }

private:
    KmpLog() : m_handle(INVALID_HANDLE_VALUE), m_index(0) {}
    HANDLE m_handle;
    int    m_index;
    std::string m_filename;
};

// Convenience macro
#define KMP_LOG(msg) kmp::KmpLog::get().log(msg)

} // namespace kmp
