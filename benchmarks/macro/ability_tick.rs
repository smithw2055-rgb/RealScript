module Bench.AbilityTick;

int applyAbility(int index, int health)
{
    int cooldown = index % 60;
    if (cooldown != 0)
        return health;
    int damage = 5 + (index % 7);
    if ((index % 5) == 0)
        damage = damage + 3;
    return health - damage;
}

int main()
{
    int ability = 0;
    int checksum = 0;
    while (ability < 5000)
    {
        checksum = checksum + applyAbility(ability, 100);
        ability = ability + 1;
    }
    return checksum;
}
