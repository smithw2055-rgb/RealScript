module Phase5.App;
import Phase5.Model;

int sumArray(int count)
{
    int[] values = new int[count];
    int index = 0;
    int sum = 0;
    while (index < count)
    {
        values[index] = index + 1;
        sum = sum + values[index];
        index = index + 1;
    }
    return sum;
}

int objectMath()
{
    Counter counter = createCounter(5);
    return counter.Add(7) + counter.Value;
}

int structMath()
{
    Pair original = new Pair(2, 3);
    Pair copy = original;
    copy.x = 10;
    return original.Sum() + copy.Sum();
}

int enumMath()
{
    Mode value = Mode.Bonus;
    if (value == Mode.Bonus)
        return 3;
    return 0;
}

long longMath()
{
    long value = 2147483648;
    return value + 2;
}

double doubleMath()
{
    return scale(4);
}

string greeting()
{
    return "hello";
}

int failDivision(int divisor)
{
    return 10 / divisor;
}

int main()
{
    return sumArray(5) + objectMath() + structMath() + enumMath();
}

int failOverflow()
{
    return 2147483647 + 1;
}

long failLongOverflow()
{
    long value = 9223372036854775807;
    return value + 1;
}

int failBounds()
{
    int[] values = new int[1];
    return values[1];
}

int failNull()
{
    Counter counter = null;
    return counter.Value;
}

int failNegativeLength()
{
    int[] values = new int[-1];
    return values.length;
}

int recurse(int value)
{
    if (value == 0)
        return 0;
    return recurse(value - 1) + 1;
}
