#include "fsnet_client.hpp"
#include "value_codec.hpp"

#include <fsnet/client.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <atomic>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <deque>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

namespace fsnet_godot {
using namespace godot;
namespace {
constexpr std::size_t MAX_PENDING = 256, MAX_NOTICES = 1024, MAX_QUEUED_BYTES = 16 * 1024 * 1024;
constexpr std::size_t MAX_PAYLOAD = 1024 * 1024;
std::atomic<uint64_t> next_session{static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) | 1};

String text(std::string_view bytes) { return String::utf8(bytes.data(), static_cast<int64_t>(bytes.size())); }
String decimal(uint64_t number) { return text(std::to_string(number)); }
std::string utf8(const String &value, std::size_t limit, const char *reason, bool nul = false) {
    if (value.length() > static_cast<int64_t>(limit)) throw std::invalid_argument(reason);
    auto encoded = value.utf8();
    if (static_cast<std::size_t>(encoded.length()) > limit) throw std::invalid_argument(reason);
    std::string result(encoded.get_data(), static_cast<std::size_t>(encoded.length()));
    if (!nul && result.find('\0') != std::string::npos) throw std::invalid_argument(reason);
    return result;
}
bool main_thread() {
    auto os = OS::get_singleton();
    if (os && os->get_thread_caller_id() == os->get_main_thread_id()) return true;
    UtilityFunctions::push_error("FsnetClient must be used on Godot's main thread");
    return false;
}
void keys(const Dictionary &options, std::initializer_list<const char *> allowed) {
    if (static_cast<std::size_t>(options.size()) > allowed.size()) throw std::invalid_argument("too many fsnet options");
    const auto names = options.keys();
    for (int64_t i = 0; i < names.size(); ++i) {
        if (names[i].get_type() != Variant::STRING && names[i].get_type() != Variant::STRING_NAME)
            throw std::invalid_argument("option keys must be strings");
        const String name = names[i]; bool found = false;
        for (auto accepted : allowed) if (name == accepted) { found = true; break; }
        if (!found) throw std::invalid_argument("unknown fsnet option");
    }
}
std::string option_string(const Dictionary &options, const char *name, std::size_t limit = 4096) {
    if (!options.has(name)) return {};
    auto value = options[name];
    if (value.get_type() != Variant::STRING) throw std::invalid_argument("fsnet text options require String");
    return utf8(value, limit, "fsnet text option too large");
}
std::chrono::milliseconds timeout(const Dictionary &options, const char *name, int64_t fallback) {
    if (!options.has(name)) return std::chrono::milliseconds(fallback);
    auto value = options[name];
    if (value.get_type() != Variant::INT) throw std::invalid_argument("timeout must be an integer");
    auto count = static_cast<int64_t>(value);
    if (count <= 0 || count > 30000) throw std::invalid_argument("timeout must be between 1 and 30000 ms");
    return std::chrono::milliseconds(count);
}
uint64_t unsigned_decimal(const Variant &value) {
    if (value.get_type() == Variant::INT) {
        auto number = static_cast<int64_t>(value);
        if (number < 0) throw std::invalid_argument("unsigned value cannot be negative");
        return static_cast<uint64_t>(number);
    }
    if (value.get_type() != Variant::STRING) throw std::invalid_argument("unsigned value requires decimal String or integer");
    auto encoded = utf8(value, 20, "invalid decimal value");
    if (encoded.empty() || (encoded.size() > 1 && encoded.front() == '0')) throw std::invalid_argument("noncanonical decimal value");
    uint64_t number = 0;
    const auto parsed = std::from_chars(encoded.data(), encoded.data() + encoded.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + encoded.size()) throw std::invalid_argument("invalid decimal value");
    return number;
}
String filesystem_path(const Dictionary &options, const char *name) {
    auto path = text(option_string(options, name));
    if (path.begins_with("res://") || path.begins_with("user://"))
        path = ProjectSettings::get_singleton()->globalize_path(path);
    return path;
}
PackedByteArray bytes(std::string_view source) {
    PackedByteArray result; result.resize(static_cast<int64_t>(source.size()));
    if (!source.empty()) std::copy(source.begin(), source.end(), result.ptrw());
    return result;
}
}

struct FsnetClient::State : std::enable_shared_from_this<State> {
    enum class Kind { connection, enter, changed, control, leave, request, watch, error };
    struct Pending { uint64_t session; bool watch = false, queued = false; };
    struct Notice {
        Kind kind = Kind::error;
        uint64_t session = 0;
        bool terminal = false;
        int64_t ticket = 0, connection_state = 0;
        fsnet::Status status = fsnet::Status::ok;
        std::string data;
        std::optional<fsnet::ClientEntity> entity;
        fsnet::ClientEvent metadata{};
        std::size_t charge = 512;
    };
    uint64_t owner = 0, session = 0;
    int64_t serial = 0;
    bool alive = true, polling = false, flushing = false, overflow = false, closed = true;
    std::size_t queued_bytes = 0, dropped = 0;
    std::string error;
    std::shared_ptr<fsnet::Client> client;
    std::deque<Notice> notices;
    std::unordered_map<int64_t, Pending> pending;

    int64_t ticket(bool watch) {
        if (pending.size() >= MAX_PENDING || serial == std::numeric_limits<int64_t>::max()) {
            fail("fsnet local request capacity exhausted"); return 0;
        }
        auto id = ++serial; pending.emplace(id, Pending{session, watch, false}); return id;
    }
    void trim_events() {
        for (auto it = notices.begin(); it != notices.end();) {
            if (it->kind == Kind::request || it->kind == Kind::watch) { ++it; continue; }
            queued_bytes -= it->charge; it = notices.erase(it); ++dropped;
        }
    }
    void discard_completion_payloads() {
        for (auto &notice : notices) {
            if (notice.kind != Kind::request && notice.kind != Kind::watch) continue;
            queued_bytes -= notice.charge;
            notice.data.clear(); notice.entity.reset(); notice.status = fsnet::Status::overloaded;
            notice.charge = 512; queued_bytes += notice.charge;
        }
    }
    void enqueue(Notice notice) {
        if (!alive) return;
        notice.charge += notice.data.size();
        if (notice.entity && notice.entity->confirmed)
            notice.charge += fsnet::attribute_image_bytes(notice.entity->confirmed->values);
        const bool completion = notice.kind == Kind::request || notice.kind == Kind::watch;
        if (notices.size() >= MAX_NOTICES || notice.charge > MAX_QUEUED_BYTES - queued_bytes) {
            overflow = true; error = "fsnet Godot event queue capacity exhausted";
            if (!completion) {
                if (notice.kind == Kind::connection && notice.terminal) trim_events();
                else { ++dropped; return; }
            }
            notice.data.clear(); notice.entity.reset(); notice.status = fsnet::Status::overloaded; notice.charge = 512;
            trim_events(); discard_completion_payloads();
        }
        queued_bytes += notice.charge; notices.push_back(std::move(notice));
    }
    void fail(std::string message) {
        error = message.substr(0, 4096);
        Notice notice; notice.kind = Kind::error; notice.session = session; notice.data = error; enqueue(std::move(notice));
    }
    Dictionary handle(fsnet::ClientEntityHandle value, uint64_t epoch) const {
        Dictionary result; result["session"] = decimal(epoch); result["generation"] = decimal(value.generation);
        result["subscription"] = decimal(value.subscription); return result;
    }
    fsnet::ClientEntityHandle parse_handle(const Dictionary &value) const {
        if (value.size() != 3 || !value.has("session") || !value.has("generation") || !value.has("subscription"))
            throw std::invalid_argument("invalid entity handle");
        if (closed || !client || client->state() != fsnet::ClientState::connected || unsigned_decimal(value["session"]) != session)
            throw std::invalid_argument("stale entity handle");
        fsnet::ClientEntityHandle result{unsigned_decimal(value["generation"]), unsigned_decimal(value["subscription"])};
        if (!result) throw std::invalid_argument("invalid entity handle");
        return result;
    }
    Dictionary image(const fsnet::ClientEntity &entity, uint64_t epoch) const {
        Dictionary result; result["id"] = text(entity.key.id); result["type"] = text(entity.key.type);
        result["handle"] = handle(entity.handle, epoch); result["automatic"] = entity.automatic;
        result["owned"] = entity.owned ? Variant(*entity.owned) : Variant(); result["controlled"] = entity.controlled;
        if (entity.confirmed) {
            result["revision"] = decimal(entity.confirmed->revision);
            result["values"] = codec::to_dictionary(entity.confirmed->values);
        } else { result["revision"] = "0"; result["values"] = Dictionary(); }
        return result;
    }
    Dictionary metadata(const fsnet::ClientEvent &event, uint64_t epoch) const {
        fsnet::ClientEntity entity{event.key, event.handle, {}, event.automatic, event.owned, event.controlled};
        return image(entity, epoch);
    }
    void complete(int64_t id, fsnet::Status status, std::string data = {},
                  std::optional<fsnet::ClientEntity> entity = {}) {
        auto found = pending.find(id); if (found == pending.end() || found->second.queued || !alive) return;
        found->second.queued = true;
        Notice notice; notice.kind = found->second.watch ? Kind::watch : Kind::request;
        notice.session = found->second.session; notice.ticket = id; notice.status = status;
        notice.data = std::move(data); notice.entity = std::move(entity); enqueue(std::move(notice));
    }
    void event(uint64_t epoch, const fsnet::ClientEvent &event) {
        if (!alive || epoch != session || closed) return;
        Notice notice; notice.session = epoch; notice.status = event.status; notice.metadata = event;
        using E = fsnet::ClientEvent::Kind;
        switch (event.kind) {
        case E::connecting: notice.kind = Kind::connection; notice.connection_state = CONNECTING; break;
        case E::connected: notice.kind = Kind::connection; notice.connection_state = CONNECTED; break;
        case E::disconnected: notice.kind = Kind::connection; notice.connection_state = DISCONNECTED; notice.terminal = true; break;
        case E::enter: notice.kind = Kind::enter; break;
        case E::changed: notice.kind = Kind::changed; break;
        case E::control: notice.kind = Kind::control; break;
        case E::leave: notice.kind = Kind::leave; notice.terminal = true; break;
        }
        if (event.kind == E::enter || event.kind == E::changed || event.kind == E::control)
            notice.entity = client->entity(event.handle);
        enqueue(std::move(notice));
    }
    void close() {
        auto original = client;
        if (!closed && original) original->disconnect(); // callbacks queue terminal metadata, never execute scripts
        closed = true; client.reset();
        for (auto &[id, request] : pending) if (!request.queued) complete(id, fsnet::Status::disconnected);
    }
    static void drive(const std::shared_ptr<State> &state) {
        if (!state->alive || state->polling || state->flushing) return;
        state->polling = true;
        auto client = state->client;
        try {
            if (client) client->poll();
            if (client && !client->last_error().empty() && state->error != client->last_error()) state->fail(client->last_error());
            if (state->overflow) {
                state->overflow = false; state->trim_events(); state->discard_completion_payloads(); state->close();
            }
        } catch (const std::exception &error) { state->fail(error.what()); state->close(); }
        state->polling = false;
        state->flushing = true;
        const auto count = std::min(state->notices.size(), MAX_NOTICES);
        for (std::size_t i = 0; i < count && state->alive && !state->notices.empty(); ++i) {
            auto notice = std::move(state->notices.front()); state->notices.pop_front(); state->queued_bytes -= notice.charge;
            auto object = Object::cast_to<FsnetClient>(ObjectDB::get_instance(state->owner));
            if (!object) break;
            const bool completion = notice.kind == Kind::request || notice.kind == Kind::watch;
            if (completion) {
                if (!state->pending.contains(notice.ticket)) continue;
                if (notice.session != state->session || state->closed) { notice.status = fsnet::Status::disconnected; notice.data.clear(); notice.entity.reset(); }
            } else if (notice.session != state->session && notice.kind != Kind::leave) continue;
            try {
                switch (notice.kind) {
                case Kind::connection: object->emit_signal("connection_state_changed", notice.connection_state, static_cast<int64_t>(notice.status)); break;
                case Kind::enter: if (notice.entity) object->emit_signal("entity_entered", state->image(*notice.entity, notice.session)); break;
                case Kind::changed: if (notice.entity) object->emit_signal("entity_changed", state->image(*notice.entity, notice.session)); break;
                case Kind::control: object->emit_signal("entity_control_changed", notice.entity ? state->image(*notice.entity, notice.session) : state->metadata(notice.metadata, notice.session)); break;
                case Kind::leave: object->emit_signal("entity_left", state->metadata(notice.metadata, notice.session)); break;
                case Kind::request: {
                    auto payload = bytes(notice.data); state->pending.erase(notice.ticket);
                    object->emit_signal("request_completed", notice.ticket, static_cast<int64_t>(notice.status), payload); break;
                }
                case Kind::watch: {
                    auto entity = notice.entity ? state->image(*notice.entity, notice.session) : Dictionary();
                    state->pending.erase(notice.ticket);
                    object->emit_signal("watch_completed", notice.ticket, static_cast<int64_t>(notice.status), entity); break;
                }
                case Kind::error: object->emit_signal("error_occurred", text(notice.data)); break;
                }
            } catch (const std::exception &error) {
                state->fail(error.what());
                auto found = state->pending.find(notice.ticket);
                if (completion && found != state->pending.end()) found->second.queued = false;
                state->close();
            }
        }
        state->flushing = false;
    }
};

FsnetClient::FsnetClient() : state_(std::make_shared<State>()) {
    state_->owner = get_instance_id();
    set_process_mode(PROCESS_MODE_ALWAYS); // RPC/socket deadlines use wall time, including a paused game tree.
    set_process(true);
}
FsnetClient::~FsnetClient() {
    auto state = std::move(state_); state->alive = false;
    if (state->client) state->client->disconnect();
    state->client.reset(); state->pending.clear(); state->notices.clear();
}
void FsnetClient::_ready() { set_process(auto_poll_); }
void FsnetClient::_process(double) { if (auto_poll_) poll(); }
void FsnetClient::set_auto_poll(bool enabled) { if (!main_thread()) return; auto_poll_ = enabled; set_process(enabled); }
bool FsnetClient::get_auto_poll() const { return main_thread() && auto_poll_; }
void FsnetClient::poll() { if (main_thread()) { auto state = state_; State::drive(state); } }
void FsnetClient::close_connection() {
    if (!main_thread()) return;
    try { state_->close(); } catch (const std::exception &error) { state_->fail(error.what()); }
}
int64_t FsnetClient::open_connection(const String &address, int64_t port, const Dictionary &options) {
    if (!main_thread()) return ERR_UNAUTHORIZED;
    auto state = state_; bool replaced = false;
    try {
        if (state->client && state->client->state() != fsnet::ClientState::disconnected) return ERR_ALREADY_IN_USE;
        if (state->notices.size() >= MAX_NOTICES / 2) return ERR_BUSY;
        if (port <= 0 || port > 65535) throw std::invalid_argument("invalid server port");
        keys(options, {"token", "ca_file", "certificate_file", "private_key_file", "server_name", "connect_timeout_ms", "request_timeout_ms"});
        fsnet::ClientOptions settings; settings.token = option_string(options, "token");
        settings.tls.ca_file = utf8(filesystem_path(options, "ca_file"), 4096, "invalid CA file path");
        settings.tls.certificate_file = utf8(filesystem_path(options, "certificate_file"), 4096, "invalid certificate file path");
        settings.tls.private_key_file = utf8(filesystem_path(options, "private_key_file"), 4096, "invalid private key file path");
        settings.tls.peer_name = option_string(options, "server_name", 256);
        settings.connect_timeout = timeout(options, "connect_timeout_ms", 5000);
        settings.request_timeout = timeout(options, "request_timeout_ms", 3000);
        const auto host = utf8(address, 256, "invalid server address");
        state->close();
        state->trim_events();
        state->discard_completion_payloads();
        // close() can itself overflow while queuing the old replicas' leave
        // events. That backpressure belongs to the retired connection; it must
        // never close a fresh session opened from a completion/leave callback.
        state->overflow = false;
        replaced = true; state->session = next_session.fetch_add(1);
        if (!state->session || state->session == std::numeric_limits<uint64_t>::max()) throw std::overflow_error("fsnet session space exhausted");
        auto client = std::make_shared<fsnet::Client>(std::move(settings));
        state->client = client; state->closed = false; state->error.clear();
        const auto epoch = state->session;
        client->on_event([weak = std::weak_ptr(state), epoch](const fsnet::ClientEvent &event) { if (auto state = weak.lock()) state->event(epoch, event); });
        client->connect({host, static_cast<uint16_t>(port), option_string(options, "server_name", 256)});
        return OK;
    } catch (const std::invalid_argument &error) { state->fail(error.what()); if (replaced) state->close(); return ERR_INVALID_PARAMETER; }
      catch (const std::exception &error) { state->fail(error.what()); state->close(); return ERR_CANT_CONNECT; }
}
int64_t FsnetClient::get_connection_state() const {
    if (!main_thread() || !state_->client) return DISCONNECTED;
    return static_cast<int64_t>(state_->client->state());
}
String FsnetClient::get_last_error() const { return main_thread() ? text(state_->error) : String("main thread required"); }
Dictionary FsnetClient::get_stats() const {
    Dictionary result; if (!main_thread()) return result;
    auto state = state_; result["state"] = get_connection_state(); result["session"] = decimal(state->session);
    result["pending_requests"] = static_cast<int64_t>(state->pending.size()); result["queued_events"] = static_cast<int64_t>(state->notices.size());
    result["queued_event_bytes"] = static_cast<int64_t>(state->queued_bytes); result["dropped_events"] = static_cast<int64_t>(state->dropped);
    if (state->client) { auto stats = state->client->stats(); result["entities"] = static_cast<int64_t>(stats.entities);
        result["generation"] = decimal(stats.generation); result["transport_pending"] = static_cast<int64_t>(stats.transport.pending);
        result["transport_queued_bytes"] = static_cast<int64_t>(stats.transport.queued_bytes); }
    return result;
}
int64_t FsnetClient::call_entity(const String &entity_id, const String &method, const PackedByteArray &payload, const Dictionary &options) {
    if (!main_thread()) return 0;
    auto state = state_; int64_t ticket = 0;
    try {
        ticket = state->ticket(false); if (!ticket) return 0;
        keys(options, {"timeout_ms", "request_id"});
        auto id = fsnet::EntityId(utf8(entity_id, 16, "invalid entity ID"));
        auto name = utf8(method, 127, "invalid entity method");
        if (payload.size() > static_cast<int64_t>(MAX_PAYLOAD)) throw std::length_error("RPC payload exceeds 1 MiB");
        fsnet::ClientRequestOptions request; request.timeout = timeout(options, "timeout_ms", 3000);
        request.request_id = option_string(options, "request_id", 128);
        if (!state->client || state->closed) { state->complete(ticket, fsnet::Status::disconnected); return ticket; }
        const auto view = std::string_view(reinterpret_cast<const char *>(payload.ptr()), static_cast<std::size_t>(payload.size()));
        state->client->call(id, name, view, [weak = std::weak_ptr(state), ticket](fsnet::Result result) {
            if (auto state = weak.lock()) state->complete(ticket, result.status, std::move(result.data));
        }, std::move(request));
    } catch (const std::exception &error) { state->fail(error.what()); if (ticket) state->complete(ticket, fsnet::Status::rejected); }
    return ticket;
}
int64_t FsnetClient::watch_entity(const String &type, const String &entity_id, int64_t timeout_ms) {
    if (!main_thread()) return 0;
    auto state = state_; int64_t ticket = 0;
    try {
        ticket = state->ticket(true); if (!ticket) return 0;
        auto name = utf8(type, 127, "invalid entity type");
        auto id = fsnet::EntityId(utf8(entity_id, 16, "invalid entity ID"));
        if (timeout_ms <= 0 || timeout_ms > 30000) throw std::invalid_argument("watch timeout must be between 1 and 30000 ms");
        if (!state->client || state->closed) { state->complete(ticket, fsnet::Status::disconnected); return ticket; }
        const auto epoch = state->session;
        state->client->watch({std::move(name), id.str()}, [weak = std::weak_ptr(state), ticket, epoch](fsnet::Status status, fsnet::ClientEntityHandle handle) {
            if (auto state = weak.lock()) {
                auto entity = state->client && !state->closed && state->session == epoch && status == fsnet::Status::ok ? state->client->entity(handle) : std::nullopt;
                state->complete(ticket, status, {}, std::move(entity));
            }
        }, std::chrono::milliseconds(timeout_ms));
    } catch (const std::exception &error) { state->fail(error.what()); if (ticket) state->complete(ticket, fsnet::Status::rejected); }
    return ticket;
}
int64_t FsnetClient::write_entity(const Dictionary &handle, const Dictionary &values, const Dictionary &options) {
    if (!main_thread()) return 0;
    auto state = state_; int64_t ticket = 0;
    try {
        ticket = state->ticket(false); if (!ticket) return 0;
        keys(options, {"expected_revision", "timeout_ms", "request_id"});
        auto native_handle = state->parse_handle(handle);
        auto native_values = codec::from_dictionary(values);
        fsnet::AttributeWriteOptions write; write.request_id = option_string(options, "request_id", 128);
        if (options.has("expected_revision")) write.expected_revision = unsigned_decimal(options["expected_revision"]);
        state->client->write(native_handle, std::move(native_values), [weak = std::weak_ptr(state), ticket](fsnet::Result result) {
            if (auto state = weak.lock()) state->complete(ticket, result.status, std::move(result.data));
        }, std::move(write), timeout(options, "timeout_ms", 3000));
    } catch (const std::exception &error) { state->fail(error.what()); if (ticket) state->complete(ticket, fsnet::Status::rejected); }
    return ticket;
}
bool FsnetClient::unwatch_entity(const Dictionary &handle) {
    if (!main_thread()) return false;
    try { auto parsed = state_->parse_handle(handle); state_->client->unwatch(parsed); return true; }
    catch (const std::exception &error) { state_->fail(error.what()); return false; }
}
Dictionary FsnetClient::get_entity(const String &entity_id) const {
    if (!main_thread() || !state_->client || state_->closed) return {};
    try { auto entity = state_->client->entity(fsnet::EntityId(utf8(entity_id, 16, "invalid entity ID")));
        return entity ? state_->image(*entity, state_->session) : Dictionary(); }
    catch (const std::exception &) { return {}; }
}
Dictionary FsnetClient::get_entity_by_handle(const Dictionary &handle) const {
    if (!main_thread()) return {};
    try { auto parsed = state_->parse_handle(handle); auto entity = state_->client->entity(parsed); return entity ? state_->image(*entity, state_->session) : Dictionary(); }
    catch (const std::exception &) { return {}; }
}
Array FsnetClient::get_entities() const {
    Array result; if (!main_thread() || !state_->client || state_->closed) return result;
    try { for (const auto &entity : state_->client->entities()) result.push_back(state_->image(entity, state_->session)); }
    catch (const std::exception &) { result.clear(); }
    return result;
}
Dictionary FsnetClient::get_controlled_entity() const {
    if (!main_thread() || !state_->client || state_->closed) return {};
    try { auto entity = state_->client->controlled(); return entity ? state_->image(*entity, state_->session) : Dictionary(); }
    catch (const std::exception &) { return {}; }
}

void FsnetClient::_bind_methods() {
    ClassDB::bind_method(D_METHOD("open_connection", "address", "port", "options"), &FsnetClient::open_connection, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("close_connection"), &FsnetClient::close_connection);
    ClassDB::bind_method(D_METHOD("poll"), &FsnetClient::poll);
    ClassDB::bind_method(D_METHOD("set_auto_poll", "enabled"), &FsnetClient::set_auto_poll);
    ClassDB::bind_method(D_METHOD("get_auto_poll"), &FsnetClient::get_auto_poll);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_poll"), "set_auto_poll", "get_auto_poll");
    ClassDB::bind_method(D_METHOD("get_connection_state"), &FsnetClient::get_connection_state);
    ClassDB::bind_method(D_METHOD("get_last_error"), &FsnetClient::get_last_error);
    ClassDB::bind_method(D_METHOD("get_stats"), &FsnetClient::get_stats);
    ClassDB::bind_method(D_METHOD("call_entity", "entity_id", "method", "payload", "options"), &FsnetClient::call_entity, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("watch_entity", "type", "entity_id", "timeout_ms"), &FsnetClient::watch_entity, DEFVAL(3000));
    ClassDB::bind_method(D_METHOD("write_entity", "handle", "values", "options"), &FsnetClient::write_entity, DEFVAL(Dictionary()));
    ClassDB::bind_method(D_METHOD("unwatch_entity", "handle"), &FsnetClient::unwatch_entity);
    ClassDB::bind_method(D_METHOD("get_entity", "entity_id"), &FsnetClient::get_entity);
    ClassDB::bind_method(D_METHOD("get_entity_by_handle", "handle"), &FsnetClient::get_entity_by_handle);
    ClassDB::bind_method(D_METHOD("get_entities"), &FsnetClient::get_entities);
    ClassDB::bind_method(D_METHOD("get_controlled_entity"), &FsnetClient::get_controlled_entity);
    ADD_SIGNAL(MethodInfo("connection_state_changed", PropertyInfo(Variant::INT, "state"), PropertyInfo(Variant::INT, "status")));
    for (auto name : {"entity_entered", "entity_changed", "entity_control_changed", "entity_left"})
        ADD_SIGNAL(MethodInfo(name, PropertyInfo(Variant::DICTIONARY, "entity")));
    ADD_SIGNAL(MethodInfo("request_completed", PropertyInfo(Variant::INT, "ticket"), PropertyInfo(Variant::INT, "status"), PropertyInfo(Variant::PACKED_BYTE_ARRAY, "data")));
    ADD_SIGNAL(MethodInfo("watch_completed", PropertyInfo(Variant::INT, "ticket"), PropertyInfo(Variant::INT, "status"), PropertyInfo(Variant::DICTIONARY, "entity")));
    ADD_SIGNAL(MethodInfo("error_occurred", PropertyInfo(Variant::STRING, "message")));
    BIND_CONSTANT(STATUS_OK); BIND_CONSTANT(STATUS_NOT_FOUND); BIND_CONSTANT(STATUS_OVERLOADED);
    BIND_CONSTANT(STATUS_TIMEOUT); BIND_CONSTANT(STATUS_DISCONNECTED); BIND_CONSTANT(STATUS_REJECTED); BIND_CONSTANT(STATUS_INTERNAL);
    BIND_CONSTANT(DISCONNECTED); BIND_CONSTANT(CONNECTING); BIND_CONSTANT(CONNECTED);
}
}
