module Phase24.Aot;

class Holder
{
    int value;
    public Holder(int initial) { value = initial; }
    public int Read() { return value; }
    public Holder Self() { return this; }
}

class BaseValue { }
class DerivedValue : BaseValue
{
    public int Read() { return 41; }
}
class OtherValue : BaseValue { }

class Settings
{
    int stored;
    public int Value { get { return stored; } set { stored = value; } }
}

struct Pair
{
    public int left;
    public int right;
}

class Recorder
{
    public int stamp;
    public int Next(int value) { stamp = stamp * 10 + value; return value; }
    public int Capture(int first, int second = 4, params int[] rest)
    {
        int total = first + second;
        int index = 0;
        while (index < rest.length) { total = total + rest[index]; index = index + 1; }
        return total;
    }
}

class OptionalBox
{
    int value;
    public OptionalBox(int initial = 6) { value = initial; }
    public int Read() { return value; }
}

class ScriptError { public int code; }
class DerivedError : ScriptError { }
class OtherError : ScriptError { }

class CleanupProbe
{
    public int value;
    public int ReturnFromTry()
    {
        try { return value; }
        finally { value = value + 1; }
    }
}

int Infer(int input)
{
    var number = input + 2;
    var values = new int[2];
    values[0] = number;
    values[1] = true ? 1 : 0;
    return values[0] + values[1];
}

int Conditional(bool condition, int divisor)
{
    return condition ? 7 : 10 / divisor;
}

Holder Maybe(bool condition)
{
    return condition ? new Holder(11) : null;
}

int Coalesce(bool condition, int divisor)
{
    var selected = Maybe(condition) ?? new Holder(10 / divisor);
    return selected.Read();
}

int NullConditional(bool condition)
{
    var input = Maybe(condition);
    var selected = input?.Self() ?? new Holder(9);
    return selected.Read();
}

int NullConditionalValue(bool condition)
{
    var input = Maybe(condition);
    return input?.Read() ?? 13;
}

bool LiftedNullableMembers(bool condition)
{
    var input = Maybe(condition);
    var lifted = input?.Read();
    return condition
        ? lifted.HasValue() && lifted.Value() == 11
        : !lifted.HasValue() && lifted.GetValueOrDefault() == 0;
}

bool RuntimeIs(bool derived)
{
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    return value is DerivedValue;
}

int RuntimeAs(bool derived)
{
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    DerivedValue selected = value as DerivedValue;
    return selected == null ? -1 : selected.Read();
}

bool TypeTokens()
{
    return typeof(DerivedValue) == typeof(DerivedValue) &&
        typeof(DerivedValue) != typeof(BaseValue);
}

int Initializers()
{
    var settings = new Settings { Value = 5 };
    var pair = new Pair { left = 2, right = 3 };
    var values = new List<int>() { 7, 11 };
    var lookup = new Dictionary<int, int>() { { 4, 13 } };
    return settings.Value + pair.left + pair.right + values.Get(1) + lookup.Get(4);
}

int Arguments()
{
    var recorder = new Recorder();
    int first = recorder.Capture(3);
    int second = recorder.Capture(second: 2, first: 1, 5, 7);
    int ordered = recorder.Capture(
        second: recorder.Next(1), first: recorder.Next(2));
    var box = new OptionalBox();
    return first + second + ordered + recorder.stamp + box.Read();
}

int PatternSwitch(bool derived)
{
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    switch (value)
    {
        case DerivedValue item when item.Read() == 41: return item.Read();
        case OtherValue other: return 2;
        default: return -1;
    }
}

int SwitchExpressions(int input, bool derived)
{
    int constant = input switch { 1 => 10, 2 when true => 20, _ => 30 };
    BaseValue value = derived ? new DerivedValue() : new OtherValue();
    int typed = value switch {
        DerivedValue item when item.Read() == 41 => item.Read(),
        OtherValue other => 2,
        _ => -1
    };
    return constant + typed;
}

void Fail(int code) { throw new DerivedError { code = code }; }

int CatchAndFinally(bool fail)
{
    int value = 1;
    try
    {
        value = 2;
        if (fail) Fail(7);
        value = 3;
    }
    catch (OtherError other) { value = 900; }
    catch (ScriptError error) { value = error.code + 10; }
    finally { value = value + 100; }
    return value;
}

int RethrowAndFinally()
{
    int value = 0;
    try
    {
        try { Fail(5); }
        catch (ScriptError inner)
        {
            value = inner.code;
            throw;
        }
    }
    catch (ScriptError outer) { value = value + outer.code * 10; }
    finally { value = value + 100; }
    return value;
}

int ReturnRunsFinally()
{
    var probe = new CleanupProbe { value = 5 };
    int returned = probe.ReturnFromTry();
    return returned * 10 + probe.value;
}

int LoopCleanup()
{
    int cleaned = 0;
    int index = 0;
    while (index < 4)
    {
        index = index + 1;
        try
        {
            if (index == 1) continue;
            if (index == 3) break;
            cleaned = cleaned + 10;
        }
        finally { cleaned = cleaned + index; }
    }
    return cleaned;
}

void Uncaught() { Fail(1); }
