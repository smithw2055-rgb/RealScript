module Bench.AllocationTick;

int main()
{
    int index = 0;
    int checksum = 0;
    while (index < 10000)
    {
        int[] values = new int[4];
        values[0] = index;
        checksum = checksum + values[0];
        index = index + 1;
    }
    return checksum;
}
