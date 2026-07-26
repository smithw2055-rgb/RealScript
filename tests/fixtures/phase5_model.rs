module Phase5.Model;

enum Mode
{
    Zero,
    Bonus = 3,
}

struct Pair
{
    int x;
    int y;

    Pair(int left, int right)
    {
        x = left;
        y = right;
    }

    int Sum()
    {
        return x + y;
    }
}

class Counter
{
    int value;

    Counter(int initial)
    {
        value = initial;
    }

    int Add(int amount)
    {
        value = value + amount;
        return value;
    }

    int Value
    {
        get { return value; }
    }
}

Counter createCounter(int initial)
{
    return new Counter(initial);
}

long widen(int value)
{
    return value;
}

double scale(long value)
{
    return value * 1.5;
}
