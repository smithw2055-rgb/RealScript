module Bench.FunctionCall;

int addOne(int value)
{
    return value + 1;
}

int main()
{
    int value = 0;
    int index = 0;
    while (index < 10000)
    {
        value = addOne(value);
        index = index + 1;
    }
    return value;
}
