module Demo.Flow;

int clamp(int value, int maximum)
{
    int result;
    if (value < 0)
    {
        result = 0;
    }
    else
    {
        result = value;
    }

    while (result > maximum)
    {
        result = result - 1;
    }

    return result;
}

bool guarded(bool enabled, int value)
{
    return enabled && value > 0;
}
