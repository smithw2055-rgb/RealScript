module Bench.EventFanout;

int notify(int emitter, int listener)
{
    return ((emitter + listener) % 3) + 1;
}

int main()
{
    int emitter = 0;
    int checksum = 0;
    while (emitter < 1000)
    {
        int listener = 0;
        while (listener < 10)
        {
            checksum = checksum + notify(emitter, listener);
            listener = listener + 1;
        }
        emitter = emitter + 1;
    }
    return checksum;
}
