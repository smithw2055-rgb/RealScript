module math_bench;

int stepCalc(int v) {
    return (v * 733 + 12345) % 1000003;
}

int computeSum(int n) {
    int acc = 1;
    int i = 0;
    while (i < n) {
        acc = stepCalc(acc + i);
        i = i + 1;
    }
    return acc;
}
