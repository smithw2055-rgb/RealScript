module Phase22.Aot;

long Schedule(long target, string callback, int delay)
{
    return target + delay;
}

bool CancelTimer(long timer)
{
    return timer != 0;
}

class Behavior
{
    int result;

    sequence Run(long target, int bonus)
    {
        int index = 0;
        int sum = 0;
        while (index < 3)
        {
            if (index == 1)
            {
                yield wait_ticks(1);
                sum = sum + 10;
            }
            else
            {
                yield wait_ticks(1);
                sum = sum + index;
            }
            index = index + 1;
        }
        result = sum + bonus;
    }

    int Read() { return result; }
}

Behavior Create() { return new Behavior(); }
