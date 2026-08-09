module Bench.AiTick;

int main()
{
    int unit = 0;
    int checksum = 0;
    while (unit < 10000)
    {
        int health = (unit % 100) + 1;
        int distance = (unit * 17) % 250;
        if (health < 25)
            checksum = checksum - 1;
        else if (distance < 80)
            checksum = checksum + 2;
        else
            checksum = checksum + 1;
        unit = unit + 1;
    }
    return checksum;
}
