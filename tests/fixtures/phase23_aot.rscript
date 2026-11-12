module Phase23.Aot;

byte EchoByte(byte value) { return value; }
uint EchoUInt(uint value) { return value; }
ulong EchoULong(ulong value) { return value; }
float EchoFloat(float value) { return value; }
char EchoChar(char value) { return value; }
long WidenByte(byte value) { return value; }
ulong WidenUInt(uint value) { return value; }
double WidenFloat(float value) { return value; }
uint AddUInt(uint left, uint right) { return left + right; }
uint AddUIntUnchecked(uint left, uint right) { return unchecked(left + right); }
ulong SubtractULong(ulong left, ulong right) { return left - right; }
float MultiplyFloat(float left, float right) { return left * right; }
byte CastByteChecked(int value) { return checked((byte)value); }
byte CastByteUnchecked(int value) { return unchecked((byte)value); }

struct MutableCounter
{
    int value;
    MutableCounter(int initial) { this.value = initial; }
    void Add(int amount) { this.value = this.value + amount; }
    int Read() { return this.value; }
}

int MutateStruct(int initial)
{
    MutableCounter counter = new MutableCounter(initial);
    counter.Add(3);
    counter.Add(4);
    return counter.Read();
}

class StructHolder
{
    public MutableCounter counter;
    public StructHolder(int initial)
    {
        counter = new MutableCounter(initial);
    }
}

int MutateStructField(int initial)
{
    StructHolder holder = new StructHolder(initial);
    holder.counter.Add(7);
    return holder.counter.Read();
}

int MutateStructIndexer(int initial)
{
    MutableCounter[] values = new MutableCounter[1];
    values[0] = new MutableCounter(initial);
    values[0].Add(8);
    return values[0].Read();
}

void Increment(ref int value) { value = value + 1; }

class IntHolder
{
    public int value;
    public IntHolder(int initial) { value = initial; }
}

int RefFieldWriteback(int initial)
{
    IntHolder holder = new IntHolder(initial);
    Increment(ref holder.value);
    return holder.value;
}

int RefIndexerWriteback(int initial)
{
    int[] values = new int[1];
    values[0] = initial;
    Increment(ref values[0]);
    return values[0];
}

int RefLocalWriteback(int initial)
{
    int value = initial;
    ref int alias = ref value;
    alias = alias + 2;
    return value;
}

int NullableRoundTrip(bool hasValue)
{
    int? value = null;
    if (hasValue)
    {
        value = 7;
    }
    if (value.HasValue())
    {
        return value.Value();
    }
    return -1;
}

Box<int> BoxInt(int value) { return new Box<int>(value); }
int UnboxInt(Box<int> value) { return value.Value(); }
int BoxRoundTrip(int value) { return UnboxInt(BoxInt(value)); }

int ReadAfterMutatingIn(in MutableCounter value)
{
    value.Add(5);
    return value.Read();
}

int InDefensiveCopy()
{
    MutableCounter value = new MutableCounter(2);
    int observed = ReadAfterMutatingIn(in value);
    return observed * 10 + value.Read();
}

ref int ForwardReference(ref int value) { return ref value; }

void MutateThroughRefReturn(ref int value)
{
    ref int alias = ref ForwardReference(ref value);
    alias = alias + 3;
}

int RefReturnWriteback(int initial)
{
    int value = initial;
    MutateThroughRefReturn(ref value);
    return value;
}
