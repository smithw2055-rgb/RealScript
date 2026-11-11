module Phase28.TrustedAsync;
import RealScript.Async;

// 1. Synchronous fast-path scalar async/await kernel (zero suspension, zero heap box)
public async Awaitable SyncTickStepAsync(int step, long[] sink) {
    await Awaitable.Completed();
    long prev = sink[0];
    int mixed = (step * 7) + 3;
    sink[0] = prev + (long)mixed;
}

public async Awaitable<int> SyncFormulaAsync(int a, int b) {
    await Awaitable.Completed();
    int left = (a * 3) + 5;
    int right = (b * 2) - 1;
    await Awaitable.Completed();
    return left + right;
}

public async Awaitable CaptureIntInline(Awaitable<int> task, int[] outSlot) {
    int v = await task;
    outSlot[0] = v;
}

public async Awaitable ConsumeCompletedVoid(Awaitable task) {
    await task;
}

public long BenchSyncScalarAsyncNoAlloc(int iterations) {
    int[] slot = new int[1];
    long acc = 0L;
    int i = 0;
    while (i < iterations) {
        Awaitable<int> t = SyncFormulaAsync(i % 256, (i + 3) % 128);
        CaptureIntInline(t, slot);
        acc = acc + (long)slot[0];
        i = i + 1;
    }
    return acc;
}

// 2. Pooled suspend/resume coroutine with 6 live scalar locals across 2 suspension points
public async Awaitable MultiStageScalarCoro(
    int seed,
    Awaitable<int> gateA,
    Awaitable<int> gateB,
    long[] outBox) {
    int s0 = seed + 11;
    int s1 = (seed * 3) + 7;
    long l0 = (long)s0 * 5L;
    double d0 = (double)seed * 1.5 + 2.0;
    bool flip = (seed % 2) == 0;
    int bonus = flip ? 19 : 23;

    int a = await gateA;
    s0 = s0 + a;
    l0 = l0 + (long)(s1 * 2);
    d0 = d0 + (double)a * 0.5;

    int b = await gateB;
    long scaledD = (long)(d0 * 2.0);
    long total = l0 + (long)(s0 + b + bonus) + scaledD;
    outBox[0] = outBox[0] + total;
}

public long BenchPooledSuspendResumeCoro(int iterations) {
    long[] outBox = new long[1];
    outBox[0] = 0L;
    int i = 0;
    while (i < iterations) {
        Awaitable<int> gateA = Async.NewIntSignal();
        Awaitable<int> gateB = Async.NewIntSignal();
        Awaitable coro = MultiStageScalarCoro(i % 64, gateA, gateB, outBox);
        Async.ResolveIntSignal(gateA, (i % 16) + 4);
        Async.StepUpdate();
        Async.ResolveIntSignal(gateB, (i % 32) + 9);
        Async.StepUpdate();
        ConsumeCompletedVoid(coro);
        i = i + 1;
    }
    return outBox[0];
}

// 3. Deep 3-level nested async scalar call chain with CancellationToken checks
public async Awaitable<int> LeafChainAsync(
    int x,
    Awaitable<int> gate,
    CancellationToken tok) {
    if (tok.IsCancellationRequested() || tok.IsTimedOut()) {
        return -1000;
    }
    int remote = await gate;
    return (x * 2) + remote + 3;
}

public async Awaitable<int> MidChainAsync(
    int x,
    Awaitable<int> gate,
    CancellationToken tok) {
    int localBias = (x % 8) + 5;
    int leafVal = await LeafChainAsync(x + localBias, gate, tok);
    await Awaitable.YieldWithToken(tok);
    return leafVal + localBias;
}

public async Awaitable RootChainAsync(
    int x,
    Awaitable<int> gate,
    CancellationToken tok,
    long[] outBox) {
    int midVal = await MidChainAsync(x, gate, tok);
    outBox[0] = outBox[0] + (long)midVal;
}

public long BenchNestedAsyncChain(int iterations) {
    CancellationTokenSource cts = CancellationTokenSource.Create();
    CancellationToken tok = cts.Token();
    long[] outBox = new long[1];
    outBox[0] = 0L;
    int i = 0;
    while (i < iterations) {
        Awaitable<int> gate = Async.NewIntSignal();
        Awaitable root = RootChainAsync(i % 32, gate, tok, outBox);
        Async.ResolveIntSignal(gate, (i % 8) + 2);
        Async.StepUpdate();
        Async.StepUpdate();
        ConsumeCompletedVoid(root);
        i = i + 1;
    }
    cts.Release();
    return outBox[0];
}

// 4. Deterministic timeout-linked cancellation token verification inside AOT
public int VerifyTimeoutLinkedTokensInAot() {
    CancellationTokenSource parent = CancellationTokenSource.Create();
    CancellationTokenSource linkedTimeout =
        CancellationTokenSource.CreateLinkedTimeout(parent.Token(), 0.25);
    CancellationToken ltTok = linkedTimeout.Token();

    if (ltTok.IsCancellationRequested() || ltTok.IsTimedOut()) {
        return -1;
    }
    Async.AdvanceTimeouts(0.10);
    if (ltTok.IsCancellationRequested() || ltTok.IsTimedOut()) {
        return -2;
    }
    int fired = Async.AdvanceTimeouts(0.20);
    if (fired < 1 || !ltTok.IsCancellationRequested() || !ltTok.IsTimedOut()) {
        return -3;
    }

    // Second pair: parent cancels BEFORE timeout -> IsCancellationRequested == true, IsTimedOut == false
    CancellationTokenSource parentB = CancellationTokenSource.Create();
    CancellationTokenSource linkedB =
        CancellationTokenSource.CreateLinkedTimeout(parentB.Token(), 5.0);
    CancellationToken tokB = linkedB.Token();
    parentB.Cancel();
    if (!tokB.IsCancellationRequested() || tokB.IsTimedOut()) {
        return -4;
    }

    // CreateLinked2 verification
    CancellationTokenSource p1 = CancellationTokenSource.Create();
    CancellationTokenSource p2 = CancellationTokenSource.Create();
    CancellationTokenSource both = CancellationTokenSource.CreateLinked2(p1.Token(), p2.Token());
    p2.Cancel();
    if (!both.Token().IsCancellationRequested() || both.Token().IsTimedOut()) {
        return -5;
    }

    both.Release();
    p2.Release();
    p1.Release();
    linkedB.Release();
    parentB.Release();
    linkedTimeout.Release();
    parent.Release();
    return 777;
}

// 5. Reference-bearing scalar coroutine (Object + Array params/locals across 2 suspensions)
public class GcPayloadBox {
    public int baseVal;
    public string tag;

    public GcPayloadBox(int b, string t) {
        baseVal = b;
        tag = t;
    }
}

public async Awaitable<int> RefAndArrayAcrossSuspendsAsync(
    GcPayloadBox boxParam,
    Awaitable<int> gateA,
    Awaitable<int> gateB) {
    int[] scratch = new int[3];
    scratch[0] = boxParam.baseVal;
    scratch[1] = 10;
    scratch[2] = 20;

    int first = await gateA;
    GcPayloadBox tempBox = new GcPayloadBox(first + scratch[0], boxParam.tag);
    scratch[1] = scratch[1] + tempBox.baseVal;

    int second = await gateB;
    scratch[2] = scratch[2] + second;
    return scratch[0] + scratch[1] + scratch[2] + boxParam.baseVal;
}

public async Awaitable<int> StartRefCoroHelper(
    int baseVal,
    Awaitable<int> gateA,
    Awaitable<int> gateB) {
    GcPayloadBox ephemeral = new GcPayloadBox(baseVal, "pinned_across_gc");
    return await RefAndArrayAcrossSuspendsAsync(ephemeral, gateA, gateB);
}

// 6. Scalar coroutine runtime fault (DivideByZero) & uncaught script exception after resume
public async Awaitable<int> FaultDivideAfterTwoSuspends(
    int denom,
    Awaitable<int> gateA,
    Awaitable<int> gateB) {
    int a = await gateA;
    int b = await gateB;
    int sum = a + b;
    return sum / denom;
}

public void ThrowBoomHelper(string detail) {
    throw new AsyncException(detail);
}

public async Awaitable<int> ThrowAfterSuspendScalarCoro(Awaitable<int> gate) {
    int v = await gate;
    if (v < 0) {
        ThrowBoomHelper("scalar_coro_uncaught_boom");
    }
    return v + 1;
}
