#pragma once

#include <fsnet/entity_schema.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace fsnet_godot::codec {

// Conversion first validates aggregate size/depth/nodes without constructing a
// native value graph, then builds the owned result. Unsupported values and
// exhausted budgets throw std::invalid_argument / std::length_error.
fsnet::EntityAttributes::Values from_dictionary(const godot::Dictionary &values);
godot::Dictionary to_dictionary(const fsnet::EntityAttributes::Values &values);
fsnet::AttributeValue from_variant(const godot::Variant &value);
godot::Variant to_variant(const fsnet::AttributeValue &value);

} // namespace fsnet_godot::codec
