# Project

- Independent Godot 4 GDExtension client plugin for fsnet.
- Link only the installed, precompiled fsnet C++ client SDK. Never include or
  fetch fsnet implementation sources as a plugin build dependency.
- GDScript and C# share one native implementation, model, codec and protocol.
- Keep game rules, entity types and presentation scenes in application scripts.
- Poll and Godot callbacks belong to the main thread. Queue native callbacks;
  guard disconnect/reconnect/free during signals and stale session handles.
- Bound conversion depth, nodes and bytes before allocating native value graphs.
- Do not commit downloaded dependencies, native binaries, credentials, .godot,
  .dev, bin/obj or release archives. Publish tested binaries as release assets.
- State tested Godot versions and platform support precisely. Commit and push
  coherent verified stages. Never force push.
