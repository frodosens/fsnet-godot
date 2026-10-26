#include "value_codec.hpp"
#include "fsnet_map.hpp"

#include <fsnet/attribute_wire.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace fsnet_godot::codec {
namespace {

constexpr std::size_t byte_limit = 1024 * 1024;
constexpr std::size_t string_limit = 65536;
constexpr std::size_t member_limit = 256;
constexpr std::size_t key_limit = 127;
constexpr unsigned depth_limit = 8;

struct Budget {
    std::size_t remaining_nodes = 4096;
    // Include the common envelope even for a standalone value. Container
    // counts use a conservative four-byte allowance during this first pass.
    std::size_t bytes = 32;

    void add(std::size_t count) {
        if (count > byte_limit - bytes) {
            throw std::length_error("fsnet value exceeds the 1 MiB aggregate limit");
        }
        bytes += count;
    }

    void node(unsigned depth, std::size_t encoded) {
        if (depth > depth_limit || !remaining_nodes) {
            throw std::length_error("fsnet value depth/node limit exceeded (8/4096)");
        }
        --remaining_nodes;
        add(encoded);
    }
};

void count_members(std::size_t count) {
    if (count > member_limit) {
        throw std::length_error("fsnet containers are limited to 256 members");
    }
}

bool valid_utf8(std::string_view value) {
    const auto *data = reinterpret_cast<const unsigned char *>(value.data());
    std::size_t index = 0;
    while (index < value.size()) {
        const auto first = data[index++];
        if (first < 0x80) {
            continue;
        }
        unsigned continuation;
        unsigned char minimum = 0x80, maximum = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation = 2;
            if (first == 0xe0) minimum = 0xa0;
            if (first == 0xed) maximum = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation = 3;
            if (first == 0xf0) minimum = 0x90;
            if (first == 0xf4) maximum = 0x8f;
        } else {
            return false;
        }
        if (continuation > value.size() - index || data[index] < minimum || data[index] > maximum) {
            return false;
        }
        ++index;
        for (unsigned i = 1; i < continuation; ++i) {
            if (data[index] < 0x80 || data[index] > 0xbf) return false;
            ++index;
        }
    }
    return true;
}

std::string text_bytes(const godot::String &text, std::size_t maximum, bool key = false) {
    // Check before UTF-8 allocation. Every valid Unicode code point needs at
    // least one encoded byte, and this bounds the temporary transcoding buffer.
    if (text.length() < 0 || static_cast<std::uint64_t>(text.length()) > maximum) {
        throw std::length_error(key ? "fsnet key exceeds 127 UTF-8 bytes" : "fsnet string exceeds 65536 bytes");
    }
    for (int64_t i = 0; i < text.length(); ++i) {
        const auto code = static_cast<std::uint32_t>(text[i]);
        if ((code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff || (key && code == 0)) {
            throw std::invalid_argument("fsnet text must contain valid Unicode; keys must not contain NUL");
        }
    }
    const auto encoded = text.utf8();
    if (static_cast<std::uint64_t>(encoded.length()) > maximum || (key && !encoded.length())) {
        throw std::length_error(key ? "fsnet keys require 1..127 UTF-8 bytes" : "fsnet string exceeds 65536 bytes");
    }
    return std::string(encoded.get_data(), static_cast<std::size_t>(encoded.length()));
}

std::string dictionary_key(const godot::Variant &key) {
    if (key.get_type() != godot::Variant::STRING && key.get_type() != godot::Variant::STRING_NAME) {
        throw std::invalid_argument("fsnet dictionaries require string keys");
    }
    return text_bytes(static_cast<godot::String>(key), key_limit, true);
}

void native_key(std::string_view key) {
    if (key.empty() || key.size() > key_limit || key.find('\0') != std::string_view::npos || !valid_utf8(key)) {
        throw std::invalid_argument("fsnet keys require nonempty UTF-8 text without NUL, at most 127 bytes");
    }
}

void finite_number(double number) {
    if (!std::isfinite(number)) {
        throw std::invalid_argument("fsnet numbers must be finite");
    }
}

void inspect(const godot::Variant &, Budget &, unsigned);

void inspect_dictionary(const godot::Dictionary &dictionary, Budget &budget, unsigned child_depth) {
    count_members(static_cast<std::size_t>(dictionary.size()));
    const godot::Array keys = dictionary.keys();
    std::set<std::string> names;
    for (int64_t i = 0; i < keys.size(); ++i) {
        const auto key = dictionary_key(keys[i]);
        if (!names.emplace(key).second) {
            throw std::invalid_argument("fsnet dictionary keys collide after text conversion");
        }
        budget.add(4 + key.size());
        inspect(dictionary[keys[i]], budget, child_depth);
    }
}

template <std::size_t N>
void inspect_components(const char *const (&names)[N], const double (&values)[N], Budget &budget, unsigned depth) {
    budget.node(depth, 5);
    for (std::size_t i = 0; i < N; ++i) {
        finite_number(values[i]);
        budget.add(4 + std::strlen(names[i]));
        budget.node(depth + 1, 9);
    }
}

template <class Matrix>
void inspect_matrix(const Matrix &matrix, Budget &budget, unsigned depth) {
    budget.node(depth, 5);
    for (int row = 0; row < 4; ++row) {
        budget.node(depth + 1, 5);
        for (int column = 0; column < 4; ++column) {
            finite_number(matrix(row, column));
            budget.node(depth + 2, 9);
        }
    }
}

void inspect(const godot::Variant &value, Budget &budget, unsigned depth) {
    using V = godot::Variant;
    switch (value.get_type()) {
        case V::BOOL: budget.node(depth, 2); return;
        case V::INT: budget.node(depth, 9); return;
        case V::FLOAT:
            finite_number(static_cast<double>(value)); budget.node(depth, 9); return;
        case V::STRING:
        case V::STRING_NAME: {
            const auto bytes = text_bytes(static_cast<godot::String>(value), string_limit);
            budget.node(depth, 5 + bytes.size()); return;
        }
        case V::PACKED_BYTE_ARRAY: {
            const godot::PackedByteArray bytes = value;
            if (static_cast<std::uint64_t>(bytes.size()) > string_limit) {
                throw std::length_error("fsnet binary strings are limited to 65536 bytes");
            }
            budget.node(depth, 5 + static_cast<std::size_t>(bytes.size())); return;
        }
        case V::DICTIONARY:
            budget.node(depth, 5); inspect_dictionary(value, budget, depth + 1); return;
        case V::ARRAY: {
            const godot::Array array = value;
            count_members(static_cast<std::size_t>(array.size()));
            budget.node(depth, 5);
            for (int64_t i = 0; i < array.size(); ++i) inspect(array[i], budget, depth + 1);
            return;
        }
        case V::OBJECT: {
            const auto *map = godot::Object::cast_to<FsnetMap>(static_cast<godot::Object *>(value));
            if (!map) throw std::invalid_argument("only FsnetMap objects can be converted to fsnet values");
            budget.node(depth, 5); inspect_dictionary(map->get_entries(), budget, depth + 1); return;
        }
        case V::VECTOR2: {
            const godot::Vector2 item = value;
            const char *names[] = {"x", "y"}; const double components[] = {item.x, item.y};
            inspect_components(names, components, budget, depth); return;
        }
        case V::VECTOR3: {
            const godot::Vector3 item = value;
            const char *names[] = {"x", "y", "z"}; const double components[] = {item.x, item.y, item.z};
            inspect_components(names, components, budget, depth); return;
        }
        case V::VECTOR4: {
            const godot::Vector4 item = value;
            const char *names[] = {"x", "y", "z", "w"}; const double components[] = {item.x, item.y, item.z, item.w};
            inspect_components(names, components, budget, depth); return;
        }
        case V::QUATERNION: {
            const godot::Quaternion item = value;
            const char *names[] = {"x", "y", "z", "w"}; const double components[] = {item.x, item.y, item.z, item.w};
            inspect_components(names, components, budget, depth); return;
        }
        case V::COLOR: {
            const godot::Color item = value;
            const char *names[] = {"r", "g", "b", "a"}; const double components[] = {item.r, item.g, item.b, item.a};
            inspect_components(names, components, budget, depth); return;
        }
        case V::PROJECTION: {
            const godot::Projection matrix = value;
            inspect_matrix([&](int row, int column) { return static_cast<double>(matrix[column][row]); }, budget, depth);
            return;
        }
        case V::TRANSFORM3D: {
            const godot::Transform3D matrix = value;
            inspect_matrix([&](int row, int column) {
                if (row == 3) return column == 3 ? 1.0 : 0.0;
                return static_cast<double>(column == 3 ? matrix.origin[row] : matrix.basis.get_column(column)[row]);
            }, budget, depth);
            return;
        }
        default: throw std::invalid_argument("unsupported Godot value type for fsnet attribute");
    }
}

fsnet::AttributeValue build(const godot::Variant &);

template <class Map>
Map build_dictionary(const godot::Dictionary &dictionary) {
    Map result;
    const godot::Array keys = dictionary.keys();
    for (int64_t i = 0; i < keys.size(); ++i) {
        result.emplace(dictionary_key(keys[i]), build(dictionary[keys[i]]));
    }
    return result;
}

template <class Matrix>
fsnet::AttributeValue build_matrix(const Matrix &matrix) {
    fsnet::AttributeArray result;
    result.reserve(4);
    for (int row = 0; row < 4; ++row) {
        fsnet::AttributeArray values;
        values.reserve(4);
        for (int column = 0; column < 4; ++column) values.emplace_back(matrix(row, column));
        result.emplace_back(std::move(values));
    }
    return fsnet::AttributeValue(std::move(result));
}

fsnet::AttributeValue build(const godot::Variant &value) {
    using V = godot::Variant;
    switch (value.get_type()) {
        case V::BOOL: return static_cast<bool>(value);
        case V::INT: return static_cast<int64_t>(value);
        case V::FLOAT: return static_cast<double>(value);
        case V::STRING:
        case V::STRING_NAME: return text_bytes(static_cast<godot::String>(value), string_limit);
        case V::PACKED_BYTE_ARRAY: {
            const godot::PackedByteArray bytes = value;
            if (!bytes.size()) return std::string{};
            return std::string(reinterpret_cast<const char *>(bytes.ptr()), static_cast<std::size_t>(bytes.size()));
        }
        case V::DICTIONARY: return build_dictionary<fsnet::AttributeStruct>(value);
        case V::ARRAY: {
            const godot::Array array = value;
            fsnet::AttributeArray result;
            result.reserve(static_cast<std::size_t>(array.size()));
            for (int64_t i = 0; i < array.size(); ++i) result.push_back(build(array[i]));
            return fsnet::AttributeValue(std::move(result));
        }
        case V::OBJECT: {
            const auto *map = godot::Object::cast_to<FsnetMap>(static_cast<godot::Object *>(value));
            return build_dictionary<fsnet::AttributeMap>(map->get_entries());
        }
        case V::VECTOR2: {
            const godot::Vector2 item = value;
            return fsnet::AttributeStruct{{"x", double(item.x)}, {"y", double(item.y)}};
        }
        case V::VECTOR3: {
            const godot::Vector3 item = value;
            return fsnet::AttributeStruct{{"x", double(item.x)}, {"y", double(item.y)}, {"z", double(item.z)}};
        }
        case V::VECTOR4: {
            const godot::Vector4 item = value;
            return fsnet::AttributeStruct{{"x", double(item.x)}, {"y", double(item.y)}, {"z", double(item.z)}, {"w", double(item.w)}};
        }
        case V::QUATERNION: {
            const godot::Quaternion item = value;
            return fsnet::AttributeStruct{{"x", double(item.x)}, {"y", double(item.y)}, {"z", double(item.z)}, {"w", double(item.w)}};
        }
        case V::COLOR: {
            const godot::Color item = value;
            return fsnet::AttributeStruct{{"r", double(item.r)}, {"g", double(item.g)}, {"b", double(item.b)}, {"a", double(item.a)}};
        }
        case V::PROJECTION: {
            const godot::Projection matrix = value;
            return build_matrix([&](int row, int column) { return static_cast<double>(matrix[column][row]); });
        }
        case V::TRANSFORM3D: {
            const godot::Transform3D matrix = value;
            return build_matrix([&](int row, int column) {
                if (row == 3) return column == 3 ? 1.0 : 0.0;
                return static_cast<double>(column == 3 ? matrix.origin[row] : matrix.basis.get_column(column)[row]);
            });
        }
        default: throw std::invalid_argument("unsupported Godot value type for fsnet attribute");
    }
}

void inspect_native(const fsnet::AttributeValue &value, Budget &budget, unsigned depth);

template <class Map>
void inspect_native_dictionary(const Map &values, Budget &budget, unsigned child_depth) {
    count_members(values.size());
    for (const auto &[key, value] : values) {
        native_key(key);
        budget.add(4 + key.size());
        inspect_native(value, budget, child_depth);
    }
}

void inspect_native(const fsnet::AttributeValue &value, Budget &budget, unsigned depth) {
    if (std::holds_alternative<std::int64_t>(value)) budget.node(depth, 9);
    else if (const auto *number = std::get_if<double>(&value)) {
        finite_number(*number); budget.node(depth, 9);
    } else if (std::holds_alternative<bool>(value)) budget.node(depth, 2);
    else if (const auto *bytes = std::get_if<std::string>(&value)) {
        if (bytes->size() > string_limit) throw std::length_error("fsnet string exceeds 65536 bytes");
        budget.node(depth, 5 + bytes->size());
    } else if (const auto *dictionary = std::get_if<fsnet::AttributeStruct>(&value)) {
        budget.node(depth, 5); inspect_native_dictionary(*dictionary, budget, depth + 1);
    } else if (const auto *array = std::get_if<fsnet::AttributeArray>(&value)) {
        count_members(array->size()); budget.node(depth, 5);
        for (const auto &child : *array) inspect_native(child, budget, depth + 1);
    } else if (const auto *map = std::get_if<fsnet::AttributeMap>(&value)) {
        budget.node(depth, 5); inspect_native_dictionary(*map, budget, depth + 1);
    } else throw std::invalid_argument("unsupported native fsnet value");
}

godot::Variant decode(const fsnet::AttributeValue &value);

template <class Map>
godot::Dictionary decode_dictionary(const Map &values) {
    godot::Dictionary result;
    for (const auto &[key, value] : values) {
        result[godot::String::utf8(key.data(), static_cast<int64_t>(key.size()))] = decode(value);
    }
    return result;
}

godot::Variant decode(const fsnet::AttributeValue &value) {
    if (const auto *integer = std::get_if<std::int64_t>(&value)) return *integer;
    if (const auto *number = std::get_if<double>(&value)) return *number;
    if (const auto *boolean = std::get_if<bool>(&value)) return *boolean;
    if (const auto *text = std::get_if<std::string>(&value)) {
        if (text->find('\0') == std::string::npos && valid_utf8(*text)) {
            return godot::String::utf8(text->data(), static_cast<int64_t>(text->size()));
        }
        godot::PackedByteArray bytes;
        bytes.resize(static_cast<int64_t>(text->size()));
        if (!text->empty()) std::memcpy(bytes.ptrw(), text->data(), text->size());
        return bytes;
    }
    if (const auto *dictionary = std::get_if<fsnet::AttributeStruct>(&value)) return decode_dictionary(*dictionary);
    if (const auto *array = std::get_if<fsnet::AttributeArray>(&value)) {
        godot::Array result;
        result.resize(static_cast<int64_t>(array->size()));
        for (std::size_t i = 0; i < array->size(); ++i) result[static_cast<int64_t>(i)] = decode((*array)[i]);
        return result;
    }
    if (const auto *map = std::get_if<fsnet::AttributeMap>(&value)) {
        return FsnetMap::from_dictionary(decode_dictionary(*map));
    }
    throw std::invalid_argument("unsupported native fsnet value");
}

} // namespace

fsnet::EntityAttributes::Values from_dictionary(const godot::Dictionary &values) {
    Budget budget;
    inspect_dictionary(values, budget, 0);
    auto result = build_dictionary<fsnet::EntityAttributes::Values>(values);
    // Public SDK validation remains authoritative. This performs no image
    // copy and catches any future change to fsnet's exact record limits.
    (void)fsnet::attribute_wire_size(result);
    return result;
}

godot::Dictionary to_dictionary(const fsnet::EntityAttributes::Values &values) {
    Budget budget;
    inspect_native_dictionary(values, budget, 0);
    return decode_dictionary(values);
}

fsnet::AttributeValue from_variant(const godot::Variant &value) {
    Budget budget;
    inspect(value, budget, 0);
    return build(value);
}

godot::Variant to_variant(const fsnet::AttributeValue &value) {
    Budget budget;
    inspect_native(value, budget, 0);
    return decode(value);
}

} // namespace fsnet_godot::codec
