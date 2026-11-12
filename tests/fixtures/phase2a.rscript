module Demo.Bytecode;

int twice(int value)
{
    return value * 2;
}

int clamp(int value, int maximum)
{
    int current = twice(value);
    while (current > maximum)
    {
        current = current - 1;
    }
    return current;
}

bool guarded(bool enabled, int value)
{
    return enabled && value > 0;
}

string choose(string value)
{
    return value;
}

string nilValue()
{
    return choose(null);
}

string greeting()
{
    return "hello";
}
