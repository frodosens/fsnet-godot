using System;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Fsnet.Godot;
using Godot;
using GDictionary = Godot.Collections.Dictionary;
using GArray = Godot.Collections.Array;

public partial class Main : Node
{
    private FsnetConnection _client = null!;

    public override async void _Ready()
    {
        try
        {
            _client = new FsnetConnection { Name = "FsnetConnection" };
            AddChild(_client);
            _client.EntityEntered += entity => GD.Print("entity entered ", entity["type"], " ", entity["id"]);
            _client.EntityControlChanged += entity =>
            {
                // Synchronous presentation/input setup. Never await gameplay
                // inside the native event callback itself.
                if (entity["controlled"].AsBool()) GD.Print("camera follow ", entity["id"]);
            };
            var args = OS.GetCmdlineUserArgs();
            if (args.Contains("--smoke")) await Smoke();
            else if (args.Contains("--integration")) await Integration();
            else
            {
                GD.Print("Set FSNET_GODOT_TEST_PORT, ENTRY_ID and TOKEN, then run with --integration; --smoke needs no server.");
                await Smoke();
            }
            GD.Print("PASS fsnet Godot C# facade");
            GetTree().Quit();
        }
        catch (Exception error)
        {
            GD.PushError(error.ToString());
            GetTree().Quit(1);
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }

    private static async Task Cancelled(Task task)
    {
        try { await task.WaitAsync(TimeSpan.FromSeconds(3)); }
        catch (OperationCanceledException) { return; }
        throw new InvalidOperationException("operation was expected to cancel");
    }

    private async Task Smoke()
    {
        Check(_client.ConnectionState == FsnetConnectionState.Disconnected, "initial state");
        Check(_client.GetEntities().Count == 0 && _client.GetControlledEntity().Count == 0, "initial views");
        var guarded = await Task.Run(() =>
        {
            try { _ = _client.GetEntities(); }
            catch (InvalidOperationException) { return true; }
            return false;
        });
        Check(guarded, "managed facade accessed Godot from a worker thread");
        var rejected = _client.RpcAsync("invalid-id", "echo", new byte[] { 0, 255 });
        Check(!rejected.IsCompleted, "native callback reentered request method");
        var result = await rejected.WaitAsync(TimeSpan.FromSeconds(3));
        Check(!result.IsSuccess, "disconnected invalid call unexpectedly succeeded");
        using (var cancellation = new CancellationTokenSource())
        {
            // Hold queued native completion until the worker has cancelled.
            // Otherwise the disconnected rejection can win on the next frame.
            _client.AutoPoll = false;
            try
            {
                var request = _client.RpcAsync("0000000000000001", "echo", cancellationToken: cancellation.Token);
                await Task.Run(cancellation.Cancel);
                await Cancelled(request);
                _client.Poll();
                Check(_client.PendingRequestCount == 0, "cancelled completion leaked its ticket");
            }
            finally { _client.AutoPoll = true; }
        }
        var closed = _client.RpcAsync("0000000000000001", "echo");
        _client.CloseConnection();
        await Cancelled(closed);
        using (var map = _client.MapValue(new GDictionary { ["potion"] = 3 }))
            Check(map.Call("get_entries").AsGodotDictionary()["potion"].AsInt64() == 3, "explicit map factory");

        var disposable = new FsnetConnection();
        AddChild(disposable);
        var abandoned = disposable.RpcAsync("0000000000000001", "echo");
        disposable.Free();
        await Cancelled(abandoned);
        GetTree().Paused = true;
        try
        {
            using var cancellation = new CancellationTokenSource();
            var paused = _client.RpcAsync("0000000000000001", "echo", cancellationToken: cancellation.Token);
            cancellation.Cancel();
            await Cancelled(paused);
            await ToSignal(GetTree().CreateTimer(.05), SceneTreeTimer.SignalName.Timeout);
            Check(_client.PendingRequestCount == 0, "cancelled native completion leaked while paused");
        }
        finally { GetTree().Paused = false; }
        GD.Print("PASS C# smoke: shared native class, queued errors, task cancellation, free and map values");
    }

    private async Task Connect(int port, string token)
    {
        var ready = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void State(long state, long status)
        {
            if (state == (long)FsnetConnectionState.Connected) ready.TrySetResult();
            else if (state == (long)FsnetConnectionState.Disconnected)
                ready.TrySetException(new InvalidOperationException("connect failed: " + status));
        }
        _client.ConnectionStateChanged += State;
        try
        {
            Check(_client.OpenConnection("127.0.0.1", port, new GDictionary { ["token"] = token }) == Error.Ok, "open connection");
            await ready.Task.WaitAsync(TimeSpan.FromSeconds(8));
        }
        finally { _client.ConnectionStateChanged -= State; }
    }

    private async Task<GDictionary> Controlled(string id)
    {
        var present = _client.GetEntity(id);
        if (present.Count != 0 && present["controlled"].AsBool()) return present;
        var ready = new TaskCompletionSource<GDictionary>(TaskCreationOptions.RunContinuationsAsynchronously);
        void Control(GDictionary entity)
        {
            if (entity["id"].AsString() == id && entity["controlled"].AsBool()) ready.TrySetResult(entity);
        }
        _client.EntityControlChanged += Control;
        try { return await ready.Task.WaitAsync(TimeSpan.FromSeconds(5)); }
        finally { _client.EntityControlChanged -= Control; }
    }

    private async Task<string> Login(string entry)
    {
        var result = await _client.RpcAsync(entry, "login", Encoding.UTF8.GetBytes("CSharp"));
        Check(result.IsSuccess, "login status " + result.Status);
        var id = Encoding.UTF8.GetString(result.Data);
        var entity = await Controlled(id);
        Check(entity["owned"].AsBool() && entity["type"].AsString() == "Player", "automatic owner/control metadata");
        return id;
    }

    private async Task Integration()
    {
        var port = int.Parse(OS.GetEnvironment("FSNET_GODOT_TEST_PORT"));
        var entry = OS.GetEnvironment("FSNET_GODOT_TEST_ENTRY_ID");
        var token = OS.GetEnvironment("FSNET_GODOT_TEST_TOKEN");
        await Connect(port, token);
        var id = await Login(entry);
        bool left = false;
        _client.EntityLeft += entity =>
        {
            if (entity["automatic"].AsBool() && entity["id"].AsString() == id) left = true;
        };
        var bytes = new byte[] { 0, 1, 255, 65, 0, 0xc3, 0x28 };
        var echoed = await _client.RpcAsync(id, "echo", bytes);
        Check(echoed.IsSuccess && echoed.Data.SequenceEqual(bytes), "binary RPC roundtrip");
        var watch = await _client.WatchEntityAsync("Player", id);
        Check(watch.IsSuccess, "manual watch");
        var handle = watch.Entity["handle"].AsGodotDictionary();
        var baselineViews = _client.GetEntities().Count;
        var cancelledWatchLeft = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        void CancelledWatch(GDictionary entity)
        {
            if (!entity["automatic"].AsBool() && entity["id"].AsString() == id) cancelledWatchLeft.TrySetResult();
        }
        _client.EntityLeft += CancelledWatch;
        try
        {
            using var cancellation = new CancellationTokenSource();
            var abandoned = _client.WatchEntityAsync("Player", id, cancellationToken: cancellation.Token);
            cancellation.Cancel();
            await Cancelled(abandoned);
            await cancelledWatchLeft.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Check(_client.GetEntities().Count == baselineViews, "cancelled watch leaked a subscription");
            Check(_client.GetControlledEntity()["id"].AsString() == id, "cancelled manual watch removed automatic player");
        }
        finally { _client.EntityLeft -= CancelledWatch; }
        var before = _client.GetEntityByHandle(handle);
        using var items = _client.MapValue(new GDictionary { ["potion"] = 3 });
        var values = new GDictionary
        {
            ["position"] = new Vector3(7, 2, -1),
            ["matrix"] = new Transform3D(Basis.Identity, new Vector3(3, 4, 5)),
            ["items"] = items,
            ["blob"] = bytes,
            ["nested"] = new GDictionary { ["label"] = "managed", ["points"] = new GArray { 1.0, 2.0, 3.0 } }
        };
        var written = await _client.WriteEntityAsync(handle, values, new GDictionary { ["expected_revision"] = before["revision"] });
        Check(written.IsSuccess, "write status " + written.Status);
        var confirmed = _client.GetEntity(id)["values"].AsGodotDictionary();
        Check(confirmed["position"].AsGodotDictionary()["x"].AsDouble() == 7, "confirmed vec3");
        Check(confirmed["matrix"].AsGodotArray()[0].AsGodotArray()[3].AsDouble() == 3, "row-major transform");
        Check(confirmed["blob"].AsByteArray().SequenceEqual(bytes), "binary attribute");
        Check(confirmed["items"].AsGodotObject().Call("get_entries").AsGodotDictionary()["potion"].AsInt64() == 3, "map wire variant");
        var sequence = confirmed["processed_input"].AsInt64() + 1;
        var moved = await _client.RpcAsync(id, "input", Encoding.UTF8.GetBytes(sequence + ":2"),
                                          new GDictionary { ["request_id"] = "csharp-input-" + sequence });
        Check(moved.IsSuccess, "input RPC");
        confirmed = _client.GetEntity(id)["values"].AsGodotDictionary();
        Check(confirmed["processed_input"].AsInt64() == sequence && confirmed["position"].AsGodotDictionary()["x"].AsDouble() == 9,
              "confirmed input acknowledgement");

        var slow = await _client.RpcAsync(id, "slow", bytes, new GDictionary { ["timeout_ms"] = 10 });
        Check(slow.Status == FsnetStatus.Timeout, "request timeout");
        var protectedRequest = _client.RpcAsync(id, "slow", bytes);
        Check(_client.OpenConnection("127.0.0.1", port, new GDictionary { ["token"] = token }) == Error.AlreadyInUse,
              "second open while connected was accepted");
        Check((await protectedRequest).IsSuccess, "rejected duplicate open cancelled current request");
        var pending = _client.RpcAsync(id, "slow", bytes);
        _client.CloseConnection();
        await Cancelled(pending);
        await Connect(port, token);
        Check(_client.GetEntityByHandle(handle).Count == 0, "old session handle still resolves");
        var stale = await _client.WriteEntityAsync(handle, new GDictionary { ["position"] = Vector3.Zero });
        Check(stale.Status == FsnetStatus.Rejected || stale.Status == FsnetStatus.Disconnected, "stale write accepted");
        Check(!_client.UnwatchEntity(handle), "stale unwatch accepted");
        await Login(entry);
        var logout = await _client.RpcAsync(entry, "logout");
        Check(logout.IsSuccess, "logout");
        Check(left && _client.GetControlledEntity().Count == 0, "logout did not revoke automatic control/presence");
        _client.CloseConnection();
        GD.Print("PASS C# integration: login, presence/control, binary RPC, map/vec3/matrix writes, input ack, timeout and reconnect fencing");
    }
}
