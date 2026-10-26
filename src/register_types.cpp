#include "register_types.hpp"
#include "fsnet_client.hpp"
#include "fsnet_map.hpp"
#include <godot_cpp/godot.hpp>

void initialize_fsnet_module(godot::ModuleInitializationLevel level) {
    if (level != godot::MODULE_INITIALIZATION_LEVEL_SCENE) return;
    godot::ClassDB::register_class<fsnet_godot::FsnetMap>();
    godot::ClassDB::register_class<fsnet_godot::FsnetClient>();
}
void uninitialize_fsnet_module(godot::ModuleInitializationLevel) {}

extern "C" {
GDExtensionBool GDE_EXPORT fsnet_library_init(GDExtensionInterfaceGetProcAddress get_proc_address,
        GDExtensionClassLibraryPtr library, GDExtensionInitialization *initialization) {
    godot::GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_fsnet_module);
    init.register_terminator(uninitialize_fsnet_module);
    init.set_minimum_library_initialization_level(godot::MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
}
