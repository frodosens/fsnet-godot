#pragma once

#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <memory>

namespace fsnet_godot {

class FsnetClient : public godot::Node {
    GDCLASS(FsnetClient, godot::Node)
    struct State;
    std::shared_ptr<State> state_;
    bool auto_poll_ = true;

protected:
    static void _bind_methods();

public:
    enum Status {
        STATUS_OK = 0, STATUS_NOT_FOUND = 1, STATUS_OVERLOADED = 2,
        STATUS_TIMEOUT = 3, STATUS_DISCONNECTED = 4, STATUS_REJECTED = 5,
        STATUS_INTERNAL = 6,
    };
    enum ConnectionState { DISCONNECTED = 0, CONNECTING = 1, CONNECTED = 2 };

    FsnetClient();
    ~FsnetClient() override;
    void _ready() override;
    void _process(double delta) override;
    int64_t open_connection(const godot::String &address, int64_t port, const godot::Dictionary &options = {});
    void close_connection();
    void poll();
    void set_auto_poll(bool enabled);
    bool get_auto_poll() const;
    int64_t get_connection_state() const;
    godot::String get_last_error() const;
    godot::Dictionary get_stats() const;
    int64_t call_entity(const godot::String &entity_id, const godot::String &method,
                       const godot::PackedByteArray &payload, const godot::Dictionary &options = {});
    int64_t watch_entity(const godot::String &type, const godot::String &entity_id, int64_t timeout_ms = 3000);
    int64_t write_entity(const godot::Dictionary &handle, const godot::Dictionary &values,
                        const godot::Dictionary &options = {});
    bool unwatch_entity(const godot::Dictionary &handle);
    godot::Dictionary get_entity(const godot::String &entity_id) const;
    godot::Dictionary get_entity_by_handle(const godot::Dictionary &handle) const;
    godot::Array get_entities() const;
    godot::Dictionary get_controlled_entity() const;
};

}
