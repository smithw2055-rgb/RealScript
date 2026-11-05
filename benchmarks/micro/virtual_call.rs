module Bench.VirtualCall;

abstract class Unit
{
    public abstract int Kind();
}

class Marine : Unit
{
    public override int Kind() { return 1; }
}

int main()
{
    Unit unit = new Marine();
    int total = 0;
    int index = 0;
    while (index < 10000)
    {
        total = total + unit.Kind();
        index = index + 1;
    }
    return total;
}
