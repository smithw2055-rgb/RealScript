module Phase20.Aot;

delegate int Reader();
delegate int Transform(int value);
delegate void Action();
delegate void Changed(int value);
delegate void Mutator(ref int value);
delegate int InReader(in int value);

interface IValue
{
    int Read();
}

class Base : IValue
{
    protected int value;
    public Base(int initial) { value = initial; }
    public virtual int Read() { return value; }
}

class Derived : Base
{
    public Derived(int initial) : base(initial) { }
    public override int Read() { return value + 2; }
    public void AddOne() { value = value + 1; }
    public void AddTen() { value = value + 10; }
}

class EventCounter
{
    event Changed Updated;
    int total;
    public EventCounter()
    {
        Updated += Add;
        Updated += value => total = total + value;
    }
    void Add(int value) { total = total + value; }
    public int Run()
    {
        Updated(3);
        Updated -= Add;
        Updated(2);
        return total;
    }
}

Reader Bind(IValue value)
{
    return value.Read;
}

int Invoke(Reader reader)
{
    return reader();
}

Transform Offset(int amount)
{
    return value => value + amount;
}

void AddTwo(ref int value) { value = value + 2; }
int ReadValue(in int value) { return value; }

int main()
{
    Derived concrete = new Derived(40);
    IValue value = concrete;
    Reader reader = Bind(value);
    Transform transform = Offset(2);
    Action one = concrete.AddOne;
    Action ten = concrete.AddTen;
    Action both = one + ten;
    both();
    Action remaining = both - ten;
    remaining();
    int referenced = 40;
    Mutator mutate = AddTwo;
    InReader readValue = ReadValue;
    mutate(ref referenced);
    EventCounter events = new EventCounter();
    return Invoke(reader) + transform(40) + concrete.Read() +
        events.Run() + readValue(in referenced) - 158;
}
