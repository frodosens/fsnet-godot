using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Gd = global::Godot;
using GDictionary = global::Godot.Collections.Dictionary;
using GArray = global::Godot.Collections.Array;

namespace Fsnet.Godot;

public enum FsnetStatus : long
{
    Ok = 0, NotFound = 1, Overloaded = 2, Timeout = 3,
    Disconnected = 4, Rejected = 5, Internal = 6
}

public enum FsnetConnectionState : long { Disconnected = 0, Connecting = 1, Connected = 2 }

public readonly record struct FsnetResult(FsnetStatus Status, byte[] Data)
{
    public bool IsSuccess => Status == FsnetStatus.Ok;
}

public readonly record struct FsnetWatchResult(FsnetStatus Status, GDictionary Entity)
{
    public bool IsSuccess => Status == FsnetStatus.Ok;
}

/// <summary>
/// Godot .NET facade over the same FsnetClient GDExtension used by GDScript.
/// Add this Node to the scene tree before using it. Methods belong to that
/// thread; Task continuations should retain Godot's synchronization context.
/// </summary>
[Gd.GlobalClass]
public partial class FsnetConnection : Gd.Node
{
    [Gd.Signal] public delegate void ConnectionStateChangedEventHandler(long state, long status);
    [Gd.Signal] public delegate void EntityEnteredEventHandler(GDictionary entity);
    [Gd.Signal] public delegate void EntityChangedEventHandler(GDictionary entity);
    [Gd.Signal] public delegate void EntityControlChangedEventHandler(GDictionary entity);
    [Gd.Signal] public delegate void EntityLeftEventHandler(GDictionary entity);
    [Gd.Signal] public delegate void ErrorOccurredEventHandler(string message);

    private const int MaxPending = 1024;
    private Gd.Node? _native;
    private int _ownerThread;
    private bool _entered;
    private readonly Dictionary<long, Pending<FsnetResult>> _requests = new();
    private readonly Dictionary<long, Pending<FsnetWatchResult>> _watches = new();
    private readonly List<(Gd.StringName Signal, Gd.Callable Callback)> _connections = new();

    private sealed class Pending<T>
    {
        internal readonly TaskCompletionSource<T> Source = new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal CancellationTokenRegistration Cancellation;
    }

    public override void _EnterTree()
    {
        _ownerThread = Environment.CurrentManagedThreadId;
        _entered = true;
        if (!Gd.ClassDB.CanInstantiate("FsnetClient"))
            throw new InvalidOperationException("FsnetClient GDExtension is not loaded. Install the native plugin matching this platform.");
        _native = Gd.ClassDB.Instantiate("FsnetClient").As<Gd.Node>();
        _native.Name = "NativeFsnetClient";
        AddChild(_native);
        Bind("connection_state_changed", Gd.Callable.From<long, long>(OnConnectionState));
        Bind("entity_entered", Gd.Callable.From<GDictionary>(entity => EmitSignal(SignalName.EntityEntered, entity)));
        Bind("entity_changed", Gd.Callable.From<GDictionary>(entity => EmitSignal(SignalName.EntityChanged, entity)));
        Bind("entity_control_changed", Gd.Callable.From<GDictionary>(entity => EmitSignal(SignalName.EntityControlChanged, entity)));
        Bind("entity_left", Gd.Callable.From<GDictionary>(entity => EmitSignal(SignalName.EntityLeft, entity)));
        Bind("error_occurred", Gd.Callable.From<string>(message => EmitSignal(SignalName.ErrorOccurred, message)));
        Bind("request_completed", Gd.Callable.From<long, long, byte[]>(OnRequestCompleted));
        Bind("watch_completed", Gd.Callable.From<long, long, GDictionary>(OnWatchCompleted));
    }

    public override void _ExitTree()
    {
        _entered = false;
        CancelPending();
        var native = _native;
        _native = null;
        if (Gd.GodotObject.IsInstanceValid(native))
        {
            foreach (var connection in _connections)
                if (native!.IsConnected(connection.Signal, connection.Callback))
                    native.Disconnect(connection.Signal, connection.Callback);
            native!.Call("close_connection");
            native.QueueFree();
        }
        _connections.Clear();
    }

    private void Bind(string signal, Gd.Callable callback)
    {
        _native!.Connect(signal, callback);
        _connections.Add((signal, callback));
    }

    private Gd.Node Native()
    {
        if (!_entered || _ownerThread != Environment.CurrentManagedThreadId)
            throw new InvalidOperationException("FsnetConnection methods require its Godot main thread and an active scene-tree Node.");
        if (!Gd.GodotObject.IsInstanceValid(_native))
            throw new ObjectDisposedException(nameof(FsnetConnection));
        return _native!;
    }

    public Gd.Error OpenConnection(string address, int port, GDictionary? options = null)
    {
        var native = Native();
        if ((FsnetConnectionState)native.Call("get_connection_state").AsInt64() != FsnetConnectionState.Disconnected)
            return Gd.Error.AlreadyInUse;
        CancelPending();
        return (Gd.Error)native.Call("open_connection", address, port, options ?? new()).AsInt64();
    }

    public void CloseConnection()
    {
        var native = Native();
        CancelPending();
        native.Call("close_connection");
    }

    public FsnetConnectionState ConnectionState => (FsnetConnectionState)Native().Call("get_connection_state").AsInt64();
    public string LastError => Native().Call("get_last_error").AsString();
    public GDictionary Stats => Native().Call("get_stats").AsGodotDictionary();
    public int PendingRequestCount { get { Native(); return _requests.Count + _watches.Count; } }
    public bool AutoPoll { get => Native().Call("get_auto_poll").AsBool(); set => Native().Call("set_auto_poll", value); }
    public void Poll() => Native().Call("poll");
    public GDictionary GetEntity(string id) => Native().Call("get_entity", id).AsGodotDictionary();
    public GDictionary GetEntityByHandle(GDictionary handle) => Native().Call("get_entity_by_handle", handle).AsGodotDictionary();
    public GDictionary GetControlledEntity() => Native().Call("get_controlled_entity").AsGodotDictionary();
    public GArray GetEntities() => Native().Call("get_entities").AsGodotArray();
    public bool UnwatchEntity(GDictionary handle) => Native().Call("unwatch_entity", handle).AsBool();

    public Task<FsnetResult> RpcAsync(string entityId, string method, byte[]? payload = null,
                                    GDictionary? options = null, CancellationToken cancellationToken = default)
    {
        var native = Native();
        if (cancellationToken.IsCancellationRequested) return Task.FromCanceled<FsnetResult>(cancellationToken);
        if (_requests.Count + _watches.Count >= MaxPending)
            return Task.FromResult(new FsnetResult(FsnetStatus.Overloaded, Array.Empty<byte>()));
        var ticket = native.Call("call_entity", entityId, method, payload ?? Array.Empty<byte>(), options ?? new()).AsInt64();
        return Track(_requests, ticket, cancellationToken);
    }

    public Task<FsnetWatchResult> WatchEntityAsync(string type, string entityId, int timeoutMs = 3000,
                                                CancellationToken cancellationToken = default)
    {
        var native = Native();
        if (cancellationToken.IsCancellationRequested) return Task.FromCanceled<FsnetWatchResult>(cancellationToken);
        if (_requests.Count + _watches.Count >= MaxPending)
            return Task.FromResult(new FsnetWatchResult(FsnetStatus.Overloaded, new()));
        var ticket = native.Call("watch_entity", type, entityId, timeoutMs).AsInt64();
        return Track(_watches, ticket, cancellationToken);
    }

    public Task<FsnetResult> WriteEntityAsync(GDictionary handle, GDictionary values,
                                            GDictionary? options = null, CancellationToken cancellationToken = default)
    {
        var native = Native();
        if (cancellationToken.IsCancellationRequested) return Task.FromCanceled<FsnetResult>(cancellationToken);
        if (_requests.Count + _watches.Count >= MaxPending)
            return Task.FromResult(new FsnetResult(FsnetStatus.Overloaded, Array.Empty<byte>()));
        var ticket = native.Call("write_entity", handle, values, options ?? new()).AsInt64();
        return Track(_requests, ticket, cancellationToken);
    }

    /// <summary>Explicit map variant, preserving map-versus-struct wire semantics.</summary>
    public Gd.GodotObject MapValue(GDictionary entries)
    {
        Native();
        var map = Gd.ClassDB.Instantiate("FsnetMap").AsGodotObject();
        map.Call("set_entries", entries);
        return map;
    }

    private Task<T> Track<T>(Dictionary<long, Pending<T>> entries, long ticket, CancellationToken token)
    {
        if (ticket <= 0) return Task.FromException<T>(new InvalidOperationException("Native fsnet request was not accepted."));
        var pending = new Pending<T>();
        entries.Add(ticket, pending);
        // Cancellation can arrive on any CLR thread. It touches only managed
        // Task state here. The admitted native operation still has to complete:
        // retain its bounded ticket until reply/timeout/close, including when
        // the game tree is paused. Never call Godot or edit dictionaries here.
        if (token.CanBeCanceled)
            pending.Cancellation = token.Register(() => pending.Source.TrySetCanceled(token));
        return pending.Source.Task;
    }

    private void OnConnectionState(long state, long status)
    {
        if (state == (long)FsnetConnectionState.Disconnected)
        {
            CancelPending();
        }
        EmitSignal(SignalName.ConnectionStateChanged, state, status);
    }

    private void OnRequestCompleted(long ticket, long status, byte[] data)
    {
        if (!_requests.Remove(ticket, out var pending)) return;
        pending.Cancellation.Dispose();
        pending.Source.TrySetResult(new((FsnetStatus)status, data));
    }

    private void OnWatchCompleted(long ticket, long status, GDictionary entity)
    {
        if (!_watches.Remove(ticket, out var pending)) return;
        pending.Cancellation.Dispose();
        if (!pending.Source.TrySetResult(new((FsnetStatus)status, entity)) && status == (long)FsnetStatus.Ok &&
            entity.ContainsKey("handle") && Gd.GodotObject.IsInstanceValid(_native))
        {
            // The local await was cancelled while subscription creation was
            // in flight. Its late success has no caller to release the handle.
            _native!.Call("unwatch_entity", entity["handle"]);
        }
    }

    private void CancelPending()
    {
        foreach (var pending in _requests.Values)
        {
            pending.Cancellation.Dispose();
            pending.Source.TrySetCanceled();
        }
        foreach (var pending in _watches.Values)
        {
            pending.Cancellation.Dispose();
            pending.Source.TrySetCanceled();
        }
        _requests.Clear();
        _watches.Clear();
    }
}
