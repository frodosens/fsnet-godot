#include "fsnet_map.hpp"

#include <godot_cpp/core/class_db.hpp>

namespace fsnet_godot {

void FsnetMap::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("set_entries", "entries"), &FsnetMap::set_entries);
    godot::ClassDB::bind_method(godot::D_METHOD("get_entries"), &FsnetMap::get_entries);
    godot::ClassDB::bind_static_method("FsnetMap", godot::D_METHOD("from_dictionary", "entries"),
            &FsnetMap::from_dictionary);
    ADD_PROPERTY(godot::PropertyInfo(godot::Variant::DICTIONARY, "entries"), "set_entries", "get_entries");
}

void FsnetMap::set_entries(const godot::Dictionary &entries) {
    entries_ = entries;
}

godot::Dictionary FsnetMap::get_entries() const {
    return entries_;
}

godot::Ref<FsnetMap> FsnetMap::from_dictionary(const godot::Dictionary &entries) {
    godot::Ref<FsnetMap> result;
    result.instantiate();
    result->set_entries(entries);
    return result;
}

} // namespace fsnet_godot
