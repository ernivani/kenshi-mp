#pragma once

// kmp_log.h — KenshiMP logging to separate per-instance file
//
// Each Kenshi instance gets its own log file: kenshimp_<PID>.log
// in the mods/KenshiMP/ directory. Also logs to Ogre::LogManager.

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
        if (m_file) return;

        // Create log file in mods/KenshiMP/ with PID
        DWORD pid = GetCurrentProcessId();
        std::ostringstream ss;
        ss << "mods\\KenshiMP\\kenshimp_" << pid << ".log";
        m_filename = ss.str();

        m_file = fopen(m_filename.c_str(), "w");
        if (m_file) {
            fprintf(m_file, "=== KenshiMP Log (PID %lu) ===\n", pid);
            fflush(m_file);
        }
    }

    void log(const std::string& msg) {
        // Write to our own file
        if (m_file) {
            // Get time
            SYSTEMTIME st;
            GetLocalTime(&st);
            fprintf(m_file, "%02d:%02d:%02d: %s\n",
                st.wHour, st.wMinute, st.wSecond, msg.c_str());
            fflush(m_file);
        }

        // Also write to Ogre log
        Ogre::LogManager::getSingleton().logMessage(msg);
    }

    void shutdown() {
        if (m_file) {
            fclose(m_file);
            m_file = NULL;
        }
    }

private:
    KmpLog() : m_file(NULL) {}
    FILE* m_file;
    std::string m_filename;
};

// Convenience macro
#define KMP_LOG(msg) kmp::KmpLog::get().log(msg)

} // namespace kmp
