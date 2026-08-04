module Phase19.Aot;

public interface IValue
{
    public int Read();
}

public abstract class Base : IValue
{
    protected int value;
    protected Base(int initial) { value = initial; }
    public abstract int Read();
}

public sealed class Derived : Base
{
    public Derived(int initial) : base(initial + 1) { }
    public override int Read() { return value + 1; }
}

int ReadInterface(IValue value)
{
    return value.Read();
}

int main()
{
    IValue value = new Derived(40);
    return ReadInterface(value);
}
