module Bench.IntegerLoop;

int main()
{
    int total = 0;
    int index = 0;
    while (index < 10000)
    {
        total = total + index;
        index = index + 1;
    }
    return total;
}
