module Bench.InterfaceCall;

interface IUnit
{
    public int Kind();
}

class Marine : IUnit
{
    public int Kind() { return 1; }
}

int main()
{
    IUnit unit = new Marine();
    int total = 0;
    int index = 0;
    while (index < 10000)
    {
        total = total + unit.Kind();
        index = index + 1;
    }
    return total;
}
