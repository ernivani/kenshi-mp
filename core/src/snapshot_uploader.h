// snapshot_uploader.h — State machine driving save → zip → upload. All
// external dependencies are injected as std::function so tests run
// synchronously without Kenshi, threads, or ENet.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace kmp {

struct SnapshotUploadAck;

class SnapshotUploader {
public:
    struct State {
        enum E {
            IDLE,
            WAIT_SAVE,
            ZIP_RUNNING,
            SEND_CHUNKS,
            AWAIT_ACK,
            FAILED,
        };
    };

    struct Deps {
        std::function<bool(const std::string& slot)>              trigger_save;
        std::function<bool()>                                     is_save_busy;
        std::function<std::string(const std::string& slot)>       resolve_slot_path;
        std::function<void(const std::string& abs_path)>          start_zip;
        std::function<bool(std::vector<uint8_t>& out)>            poll_zip;
        std::function<bool(const uint8_t* data, size_t len)>      send_reliable;
        std::function<float()>                                    now_seconds;
    };

    explicit SnapshotUploader(Deps deps);

    void start(const std::string& slot_name);
    void tick(float dt);
    void on_ack(const SnapshotUploadAck& ack);

    State::E           state() const;
    const std::string& last_error() const;
    std::string        progress_text() const;

private:
    Deps m_deps;
    State::E m_state;
    std::string m_slot_name;
    std::string m_slot_path;

    float m_enter_wait_t;
    float m_enter_ack_t;

    std::vector<uint8_t> m_blob;

    uint32_t m_upload_id;
    uint64_t m_offset;

    std::string m_error;

    void go_failed(const std::string& err);
    void start_sending_begin();
    void send_next_chunks();
    void send_end();
};

} // namespace kmp
