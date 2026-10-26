// Disposable protocol fixture. Game rules belong to application scripts; these
// native handlers exercise the shipped SDK without Lua or fsnet source includes.
#include <fsnet/attribute_replication.hpp>
#include <fsnet/database.hpp>
#include <fsnet/entity_host.hpp>
#include <fsnet/entity_message.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using namespace fsnet;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
volatile std::sig_atomic_t running = 1;
void stop_signal(int) { running = 0; }

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::int64_t integer(std::string_view value) {
    std::int64_t number = 0;
    auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size())
        throw std::invalid_argument("invalid fixture integer");
    return number;
}
AttributeDefinition attribute(AttributeType type, AttributeValue initial, bool writable = false) {
    AttributeDefinition result{type, std::move(initial)};
    result.client = writable ? Access::read_write : Access::read;
    result.visibility = Visibility::observers;
    return result;
}
AttributeDefinition number() { return attribute(AttributeType::number, 0.0, true); }
AttributeDefinition tuple(std::initializer_list<const char*> fields) {
    auto result = attribute(AttributeType::structure, AttributeStruct{}, true);
    AttributeStruct initial;
    for (auto name : fields) {
        result.fields.emplace(name, number());
        initial.emplace(name, 0.0);
    }
    result.initial = std::move(initial);
    return result;
}
AttributeDefinition array(AttributeDefinition element, std::size_t count) {
    auto result = attribute(AttributeType::array, AttributeArray(count, element.initial), true);
    result.length = count;
    result.element = std::make_shared<const AttributeDefinition>(std::move(element));
    return result;
}
EntitySchema player_schema() {
    EntitySchema schema;
    schema.attributes.emplace("name", attribute(AttributeType::string, std::string("Player")));
    schema.attributes.emplace("created", attribute(AttributeType::integer, std::int64_t(1)));
    schema.attributes.emplace("processed_input", attribute(AttributeType::integer, std::int64_t(0)));
    schema.attributes.emplace("enabled", attribute(AttributeType::boolean, true, true));
    schema.attributes.emplace("count", attribute(AttributeType::integer, std::int64_t(0), true));
    schema.attributes.emplace("weight", number());
    schema.attributes.emplace("position", tuple({"x", "y", "z"}));
    auto matrix = array(array(number(), 4), 4);
    auto& rows = std::get<AttributeArray>(matrix.initial);
    for (std::size_t i = 0; i != 4; ++i) std::get<AttributeArray>(rows[i])[i] = 1.0;
    schema.attributes.emplace("matrix", std::move(matrix));
    auto items = attribute(AttributeType::map, AttributeMap{}, true);
    items.element = std::make_shared<const AttributeDefinition>(attribute(AttributeType::integer, std::int64_t(0), true));
    items.max_items = 8;
    schema.attributes.emplace("items", std::move(items));
    schema.attributes.emplace("blob", attribute(AttributeType::string, std::string{}, true));
    auto nested = attribute(AttributeType::structure, AttributeStruct{}, true);
    nested.fields.emplace("label", attribute(AttributeType::string, std::string("fixture"), true));
    nested.fields.emplace("points", array(number(), 3));
    nested.initial = AttributeStruct{{"label", std::string("fixture")}, {"points", AttributeArray{0.0, 0.0, 0.0}}};
    schema.attributes.emplace("nested", std::move(nested));
    schema.attributes.emplace("vector2", tuple({"x", "y"}));
    schema.attributes.emplace("vector4", tuple({"x", "y", "z", "w"}));
    auto rotation = tuple({"x", "y", "z", "w"});
    std::get<AttributeStruct>(rotation.initial).at("w") = 1.0;
    schema.attributes.emplace("rotation", std::move(rotation));
    auto color = tuple({"r", "g", "b", "a"});
    std::get<AttributeStruct>(color.initial).at("a") = 1.0;
    schema.attributes.emplace("color", std::move(color));
    schema.validate();
    return schema;
}
std::string initial(const EntitySchema& schema) {
    EntityAttributes model(schema, "");
    return encode_attributes({0, 0, true, model.all_values()});
}
struct Input {
    std::string id, state, payload;
    AttributeUpdate image;
};
Input parse(std::string input) {
    auto entity = parse_entity_message_view(input);
    auto state = entity ? parse_state_message_view(entity->payload) : std::nullopt;
    if (!entity || !state) throw std::invalid_argument("invalid fixture request");
    return {std::string(entity->id), std::string(state->state), std::string(state->payload), decode_attributes(state->state)};
}
Result unchanged(const Input& input, std::string payload = {}) {
    return {Status::ok, state_message(input.state, payload)};
}
Result changed(Input input, std::string payload = {}) {
    if (input.image.revision == UINT64_MAX) throw std::overflow_error("fixture revision exhausted");
    ++input.image.revision;
    return {Status::ok, state_message(encode_attributes(input.image), payload)};
}

class Fixture {
    struct Deferred {
        Clock::time_point due;
        CallIdentity identity;
        Reply reply;
        Result result;
    };
    Node node_;
    std::shared_ptr<Storage> storage_ = memory_database();
    std::shared_ptr<EntityScriptHost> host_;
    std::unique_ptr<AttributeReplication> replication_;
    std::unordered_map<std::string, std::string> players_;
    std::vector<Deferred> deferred_;
    std::string hub_id_;

    void bind(const std::string& type, const std::string& method, const std::string& state,
              RpcPolicy policy, std::function<void(const CallIdentity&, Input, Reply)> handler) {
        host_->bind_stateful_context(type + "." + method, type, state, policy,
            [handler = std::move(handler)](const CallIdentity& who, std::string bytes, Reply reply) {
                try { handler(who, parse(std::move(bytes)), reply); }
                catch (const std::exception&) { reply({Status::rejected, {}}); }
            });
    }
    void login(const CallIdentity& who, Input input, Reply reply) {
        if (!who.connection || input.payload.empty() || input.payload.size() > 32)
            return reply({Status::rejected, {}});
        const auto name = input.payload;
        host_->create_entity("Player", {}, {"godot-fixture", "player:" + who.subject, {}, "godot-fixture"},
            [this, who, input = std::move(input), name, reply](Result created) mutable {
                if (created.status != Status::ok) return reply(std::move(created));
                if (!who.live()) return reply({Status::disconnected, {}});
                const auto id = created.data;
                host_->bind_entity_owner("Player", id, who.connection,
                    [this, who, input = std::move(input), name, id, reply](Result bound) mutable {
                        if (bound.status != Status::ok) return reply(std::move(bound));
                        if (!who.live()) return reply({Status::disconnected, {}});
                        host_->call_entity("@", entity_call_message(EntityId(id), "configure", name), 1s,
                            [this, who, input = std::move(input), id, reply](Result configured) mutable {
                                if (configured.status != Status::ok) return reply(std::move(configured));
                                if (!who.live()) return reply({Status::disconnected, {}});
                                host_->entity_visibility(who.peer, "Player", id, 1,
                                    [this, who, input = std::move(input), id, reply](Result shown) mutable {
                                        if (shown.status != Status::ok) return reply(std::move(shown));
                                        if (!who.live()) return reply({Status::disconnected, {}});
                                        host_->entity_visibility(who.peer, "Player", id, 3,
                                            [this, who, input = std::move(input), id, reply](Result possessed) mutable {
                                                if (possessed.status != Status::ok) return reply(std::move(possessed));
                                                if (!who.live()) return reply({Status::disconnected, {}});
                                                players_.insert_or_assign(who.connection.key(), id);
                                                reply(unchanged(input, id));
                                            });
                                    });
                            });
                    });
            });
    }
    void logout(const CallIdentity& who, Input input, Reply reply) {
        const auto found = players_.find(who.connection.key());
        if (found == players_.end()) return reply(unchanged(input));
        const auto id = found->second;
        host_->entity_visibility(who.peer, "Player", id, 4,
            [this, who, input = std::move(input), id, reply](Result unpossessed) mutable {
                if (unpossessed.status != Status::ok) return reply(std::move(unpossessed));
                host_->entity_visibility(who.peer, "Player", id, 2,
                    [this, who, input = std::move(input), id, reply](Result hidden) mutable {
                        if (hidden.status != Status::ok) return reply(std::move(hidden));
                        host_->bind_entity_owner("Player", id, {},
                            [this, who, input = std::move(input), reply](Result unbound) mutable {
                                if (unbound.status != Status::ok) return reply(std::move(unbound));
                                players_.erase(who.connection.key());
                                reply(unchanged(input));
                            });
                    });
            });
    }
public:
    explicit Fixture(std::string token) : node_(std::move(token)) {
        // Every local test certificate/peer is intentionally the same fixture
        // account. Production must provide its own authenticated identity policy.
        node_.identify([](auto, const auto&) -> std::optional<CallIdentity> {
            return CallIdentity{Caller::client, "fixture"};
        });
        EntityRuntimeLimits limits;
        limits.request_timeout = 2s;
        host_ = std::make_shared<EntityScriptHost>(std::make_shared<NodeScriptHost>(node_), storage_,
            "godot-fixture/" + ConnectionRef::create().id, limits);
        const auto schema = player_schema();
        host_->define_schema("Player", schema);
        const auto state = initial(schema);
        bind("Player", "echo", state, {true, false, false}, [](const auto&, Input input, Reply reply) {
            reply(unchanged(input, input.payload));
        });
        bind("Player", "describe", state, {true, false, true}, [](const auto&, Input input, Reply reply) {
            reply(unchanged(input, std::get<std::string>(input.image.values.at("name"))));
        });
        bind("Player", "configure", state, {false, true, false}, [](const auto&, Input input, Reply reply) {
            if (input.payload.empty() || input.payload.size() > 32) return reply({Status::rejected, {}});
            input.image.values.at("name") = input.payload;
            reply(changed(std::move(input)));
        });
        bind("Player", "input", state, {true, false, true}, [](const auto&, Input input, Reply reply) {
            const auto split = input.payload.find(':');
            if (split == std::string::npos) return reply({Status::rejected, {}});
            const auto sequence = integer(std::string_view(input.payload).substr(0, split));
            const auto delta = integer(std::string_view(input.payload).substr(split + 1));
            auto& processed = std::get<std::int64_t>(input.image.values.at("processed_input"));
            if (sequence == processed) return reply(unchanged(input, std::to_string(processed)));
            if (sequence <= 0 || processed == INT64_MAX || sequence != processed + 1 || delta < -10 || delta > 10)
                return reply({Status::rejected, {}});
            processed = sequence;
            auto& position = std::get<AttributeStruct>(input.image.values.at("position"));
            std::get<double>(position.at("x")) += static_cast<double>(delta);
            reply(changed(std::move(input), std::to_string(sequence)));
        });
        bind("Player", "$write", state, {true, false, true}, [this, schema](const auto& who, Input input, Reply reply) {
            EntityAttributes model(schema, host_->owner_binding("Player", input.id));
            model.restore_full(input.image.values, input.image.revision);
            auto proposed = stage_attribute_proposal(model, decode_attributes(input.payload), who);
            if (!model.publish(proposed)) return reply({Status::rejected, {}});
            reply({Status::ok, state_message(encode_attributes({0, model.revision(), true, model.all_values()}), "")});
        });
        auto slow = [this](const auto& who, Input input, Reply reply) {
            deferred_.push_back({Clock::now() + 200ms, who, std::move(reply), unchanged(input, input.payload)});
        };
        bind("Player", "slow", state, {true, false, true}, slow);
        host_->define_schema("Hub", {});
        const auto hub_state = initial({});
        bind("Hub", "login", hub_state, {true, false, false},
            [this](const auto& who, Input input, Reply reply) { login(who, std::move(input), std::move(reply)); });
        bind("Hub", "logout", hub_state, {true, false, false},
            [this](const auto& who, Input input, Reply reply) { logout(who, std::move(input), std::move(reply)); });
        bind("Hub", "discover", hub_state, {true, false, false}, [](const auto&, Input input, Reply reply) {
            reply(unchanged(input, input.id));
        });
        bind("Hub", "echo", hub_state, {true, false, false}, [](const auto&, Input input, Reply reply) {
            reply(unchanged(input, input.payload));
        });
        bind("Hub", "slow", hub_state, {true, false, false}, slow);
        replication_ = std::make_unique<AttributeReplication>(node_, host_);
        std::optional<Result> created;
        host_->create_entity("Hub", {}, {"godot-fixture", "entry", {}, "godot-fixture"},
            [&](Result result) { created = std::move(result); });
        const auto deadline = Clock::now() + 5s;
        while (!created) {
            poll();
            require(Clock::now() < deadline, "fixture bootstrap timed out");
            std::this_thread::sleep_for(1ms);
        }
        require(created->status == Status::ok, "fixture entry creation failed");
        hub_id_ = created->data;
    }
    ~Fixture() {
        replication_->stop();
        host_->stop();
        deferred_.clear();
        node_.stop();
    }
    void poll() {
        node_.poll();
        host_->poll();
        replication_->poll();
        const auto now = Clock::now();
        std::vector<Deferred> ready;
        for (auto it = deferred_.begin(); it != deferred_.end();) {
            if (it->due <= now) { ready.push_back(std::move(*it)); it = deferred_.erase(it); }
            else ++it;
        }
        for (auto& operation : ready)
            operation.reply(operation.identity.live() ? std::move(operation.result) : Result{Status::disconnected, {}});
    }
    void listen(unsigned port) {
        const auto assigned = node_.listen("127.0.0.1", static_cast<std::uint16_t>(port));
        std::cout << "{\"port\":" << assigned << ",\"entry_id\":\"" << hub_id_ << "\"}" << std::endl;
    }
};
}

int main(int argc, char** argv) {
    try {
        const char* configured = std::getenv("FSNET_GODOT_TEST_TOKEN");
        std::string token = configured ? configured : "";
        unsigned port = 0;
        std::chrono::seconds lifetime{120};
        for (int i = 1; i < argc; ++i) {
            const std::string_view option(argv[i]);
            if (i + 1 >= argc) throw std::invalid_argument("fixture option needs a value");
            if (option == "--token") token = argv[++i];
            else if (option == "--port") {
                const auto value = integer(argv[++i]);
                if (value < 0 || value > 65535) throw std::invalid_argument("fixture port out of range");
                port = static_cast<unsigned>(value);
            } else if (option == "--timeout-seconds") {
                const auto value = integer(argv[++i]);
                if (value <= 0 || value > 600) throw std::invalid_argument("fixture lifetime out of range");
                lifetime = std::chrono::seconds(value);
            } else throw std::invalid_argument("unknown fixture option");
        }
        std::signal(SIGINT, stop_signal);
        std::signal(SIGTERM, stop_signal);
        Fixture fixture(std::move(token));
        fixture.listen(port);
        const auto deadline = Clock::now() + lifetime;
        while (running && Clock::now() < deadline) {
            fixture.poll();
            std::this_thread::sleep_for(1ms);
        }
        if (running) throw std::runtime_error("fixture lifetime exceeded");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
