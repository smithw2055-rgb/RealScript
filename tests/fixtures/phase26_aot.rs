module Phase26.Aot;

public class RpcError {
    public int code;
    public string detail;

    public RpcError(int c, string d) {
        code = c;
        detail = d;
    }
}

public interface IAsyncStage {
    Awaitable<int> ComputeAsync(int seed, Awaitable<int> gate);
}

public class MultiplierStage : IAsyncStage {
    public int factor;
    public string tag;

    public MultiplierStage(int f, string t) {
        factor = f;
        tag = t;
    }

    public virtual async Awaitable<int> ComputeAsync(int seed, Awaitable<int> gate) {
        string localPrefix = tag + ":ok";
        int remote = await gate;
        return (seed + remote) * factor + localPrefix.Length;
    }
}

public class BonusStage : MultiplierStage {
    public int bonus;

    public BonusStage(int f, string t, int b) : base(f, t) {
        bonus = b;
    }

    public override async Awaitable<int> ComputeAsync(int seed, Awaitable<int> gate) {
        int baseVal = await base.ComputeAsync(seed, gate);
        return baseVal + bonus;
    }
}

public async Awaitable<int> SyncFastAdd(int a, int b) {
    int x = await Awaitable<int>.FromResult(a * 2);
    await Awaitable.Completed();
    return x + b;
}

public int RunSyncFastPath(int a, int b) {
    Awaitable<int> task = SyncFastAdd(a, b);
    if (!task.IsCompleted()) {
        return -1;
    }
    return RunConsumeSync(task);
}

public async Awaitable<int> ConsumeIntTask(Awaitable<int> t) {
    int v = await t;
    return v;
}

public int RunConsumeSync(Awaitable<int> t) {
    // Synchronously completed Awaitable<int> resolves inline via helper box
    int[] box = new int[1];
    box[0] = -999;
    StartCaptureInt(t, box);
    return box[0];
}

public async Awaitable StartCaptureInt(Awaitable<int> t, int[] box) {
    int v = await t;
    box[0] = v;
}

public async Awaitable<int> DeepChainLeaf(Awaitable<int> sig, int delta) {
    int v = await sig;
    return v + delta;
}

public async Awaitable<int> DeepChainMid(Awaitable<int> sig, int mul) {
    int a = await DeepChainLeaf(sig, 5);
    await Awaitable.Yield();
    return a * mul;
}

public async Awaitable DeepChainRoot(Awaitable<int> sig, int[] outBox) {
    IAsyncStage stage = new BonusStage(3, "node", 7);
    Awaitable<int> gate = Async.NewIntSignal();
    Awaitable<int> polyTask = stage.ComputeAsync(4, gate);
    int mid = await DeepChainMid(sig, 2);
    Async.ResolveIntSignal(gate, 6);
    int poly = await polyTask;
    outBox[0] = mid + poly;
}

public int RunDeepChainAndPolymorphism(int inputVal) {
    Awaitable<int> sig = Async.NewIntSignal();
    int[] outBox = new int[1];
    outBox[0] = 0;
    Awaitable root = DeepChainRoot(sig, outBox);
    if (root.IsCompleted()) {
        return -10;
    }
    Async.ResolveIntSignal(sig, inputVal);
    // Tick 1 resolves sig -> DeepChainLeaf -> DeepChainMid hits Yield()
    Async.StepUpdate();
    if (root.IsCompleted()) {
        return -20;
    }
    // Tick 2 resolves Yield() -> gate resolved -> BonusStage completes -> DeepChainRoot completes!
    Async.StepUpdate();
    if (!root.IsCompleted()) {
        return -30;
    }
    root.Release();
    return outBox[0];
}

public async Awaitable WorkerIncrement(Awaitable gate, int[] log, int slot, int val) {
    await gate;
    log[slot] = val;
}

public async Awaitable CancellableLoser(CancellationToken token, int[] log, int slot) {
    try {
        await Awaitable.YieldWithToken(token);
        log[slot] = 999;
    } catch (AsyncCanceledException ex) {
        log[slot] = -77;
    } finally {
        log[slot + 1] = log[slot + 1] + 10;
    }
}

public async Awaitable CombinatorCoordinator(
    Awaitable g0,
    Awaitable g1,
    Awaitable raceFast,
    int[] log) {
    Awaitable[] batch = new Awaitable[2];
    batch[0] = WorkerIncrement(g0, log, 0, 11);
    batch[1] = WorkerIncrement(g1, log, 1, 22);
    await Async.WhenAll(batch);

    CancellationTokenSource loserCts = CancellationTokenSource.Create();
    Awaitable[] race = new Awaitable[2];
    race[0] = CancellableLoser(loserCts.Token(), log, 2);
    race[1] = raceFast;
    int winner = await Async.WhenAnyRace(race, CancellationToken.None(), loserCts);
    log[4] = winner;
    loserCts.Release();
}

public int RunCombinatorsAndRace() {
    Awaitable g0 = Async.NewSignal();
    Awaitable g1 = Async.NewSignal();
    Awaitable raceFast = Async.NewSignal();
    int[] log = new int[5];
    Awaitable coord = CombinatorCoordinator(g0, g1, raceFast, log);

    Async.ResolveSignal(g0);
    Async.StepUpdate();
    if (coord.IsCompleted()) {
        return -1;
    }
    Async.ResolveSignal(g1);
    Async.StepUpdate();
    // Now inside WhenAnyRace; resolve raceFast (index 1) so loser (index 0) is auto-canceled!
    Async.ResolveSignal(raceFast);
    Async.StepUpdate();
    if (!coord.IsCompleted()) {
        return -2;
    }
    coord.Release();
    // log[0]=11, log[1]=22, log[2]=-77, log[3]=10, log[4]=1 => 11 + 22 - 77 + 10 + 100*1 = 66
    return log[0] + log[1] + log[2] + log[3] + log[4] * 100;
}

public async Awaitable<int> FailingRemoteOp(Awaitable trigger, bool shouldThrow, int code) {
    await trigger;
    if (shouldThrow) {
        throw new RpcError(code, "upstream-reset");
    }
    return code;
}

public async Awaitable CrossAwaitTryCatchFinally(
    Awaitable trigger,
    Awaitable cleanupGate,
    bool shouldThrow,
    int[] trace) {
    try {
        trace[0] = 1;
        int ok = await FailingRemoteOp(trigger, shouldThrow, 42);
        trace[1] = ok;
    } catch (RpcError err) {
        trace[1] = -err.code;
        await Awaitable.Yield();
        trace[2] = err.detail.Length;
    } finally {
        await cleanupGate;
        trace[3] = trace[3] + 1000;
    }
}

public int RunCrossAwaitExceptionHandling(bool shouldThrow) {
    Awaitable trigger = Async.NewSignal();
    Awaitable cleanupGate = Async.NewSignal();
    int[] trace = new int[4];
    Awaitable task = CrossAwaitTryCatchFinally(trigger, cleanupGate, shouldThrow, trace);

    Async.ResolveSignal(trigger);
    Async.StepUpdate();
    if (shouldThrow) {
        // Suspended on Yield() inside catch block!
        Async.StepUpdate();
    }
    if (task.IsCompleted()) {
        return -1;
    }
    // Suspended on cleanupGate inside finally block!
    Async.ResolveSignal(cleanupGate);
    Async.StepUpdate();
    if (!task.IsCompleted()) {
        return -2;
    }
    task.Release();
    // When shouldThrow=true: trace = [1, -503, 14, 1000] (wait: code passed above is 42 -> -42 + 1 + 14 + 1000 = 973)
    // When shouldThrow=false: trace = [1, 42, 0, 1000] -> 1043
    return trace[0] + trace[1] + trace[2] + trace[3];
}

public async Awaitable UncaughtWithAsyncFinally(Awaitable trigger, Awaitable cleanupGate, int[] log) {
    try {
        await trigger;
        throw new RpcError(504, "gateway-timeout");
    } finally {
        await cleanupGate;
        log[0] = 777;
    }
}

public async Awaitable ObserveUncaughtFault(Awaitable trigger, Awaitable cleanupGate, int[] log) {
    try {
        await UncaughtWithAsyncFinally(trigger, cleanupGate, log);
        log[1] = -1;
    } catch (RpcError e) {
        log[1] = e.code + e.detail.Length;
    }
}

public int RunAsyncFinallyRethrowPropagation() {
    Awaitable trigger = Async.NewSignal();
    Awaitable cleanupGate = Async.NewSignal();
    int[] log = new int[2];
    Awaitable obs = ObserveUncaughtFault(trigger, cleanupGate, log);

    Async.ResolveSignal(trigger);
    Async.StepUpdate();
    if (obs.IsCompleted() || log[0] != 0) {
        return -10;
    }
    Async.ResolveSignal(cleanupGate);
    Async.StepUpdate();
    if (!obs.IsCompleted()) {
        return -20;
    }
    obs.Release();
    // log[0] = 777 (from inner async finally), log[1] = 504 + 15 = 519 => 1296
    return log[0] + log[1];
}

public async Awaitable HostDrivenWorker(Awaitable<int> gate, int[] box) {
    int first = await gate;
    await Awaitable.Yield();
    box[0] = first * 10 + 3;
}

public async Awaitable<int> BenchSyncLeaf(int x) {
    int a = await Awaitable<int>.FromResult(x + 3);
    await Awaitable.Completed();
    return a * 2;
}

public async Awaitable BenchSyncRunner(int iterations) {
    int acc = 0;
    for (int i = 0; i < iterations; i = i + 1) {
        int val = await BenchSyncLeaf(i);
        if (val > 0) {
            acc = acc + 1;
        }
    }
}

public int BenchSyncFastPath(int iterations) {
    Awaitable t = BenchSyncRunner(iterations);
    return iterations;
}

public async Awaitable BenchSuspendOnce(Awaitable sig, int[] counter) {
    int localScalar = counter[0] + 1;
    await sig;
    counter[0] = localScalar;
}

public int BenchPooledSuspend(int iterations) {
    int[] counter = new int[1];
    counter[0] = 0;
    for (int i = 0; i < iterations; i = i + 1) {
        Awaitable sig = Async.NewSignal();
        Awaitable task = BenchSuspendOnce(sig, counter);
        Async.ResolveSignal(sig);
        Async.StepUpdate();
        task.Release();
    }
    return counter[0];
}

public async Awaitable BenchProtectedSuspendOnce(Awaitable sig, int[] counter) {
    try {
        await sig;
        counter[0] = counter[0] + 1;
    } finally {
        counter[0] = counter[0] + 0;
    }
}

public int BenchProtectedTryFinallySuspend(int iterations) {
    int[] counter = new int[1];
    counter[0] = 0;
    for (int i = 0; i < iterations; i = i + 1) {
        Awaitable sig = Async.NewSignal();
        Awaitable task = BenchProtectedSuspendOnce(sig, counter);
        Async.ResolveSignal(sig);
        Async.StepUpdate();
        task.Release();
    }
    return counter[0];
}
