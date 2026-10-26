#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

namespace fsnet_godot {

// A dictionary alone cannot retain the distinction between a fixed struct and
// a dynamic fsnet map. This wrapper carries that wire type without reserving a
// magic key in application data. Validation occurs at the network boundary.
class FsnetMap : public godot::RefCounted {
    GDCLASS(FsnetMap, godot::RefCounted)

    godot::Dictionary entries_;

protected:
    static void _bind_methods();

public:
    void set_entries(const godot::Dictionary &entries);
    godot::Dictionary get_entries() const;
    static godot::Ref<FsnetMap> from_dictionary(const godot::Dictionary &entries);
};

} // namespace fsnet_godot
