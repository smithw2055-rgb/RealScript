module Phase21.Aot;

class RangeEnumerator
{
    int current;
    int limit;
    RangeEnumerator(int end) { current = -1; limit = end; }
    bool MoveNext() { current = current + 1; return current < limit; }
    int Current() { return current; }
}

class Range
{
    int limit;
    Range(int end) { limit = end; }
    RangeEnumerator GetEnumerator() { return new RangeEnumerator(limit); }
}

interface IValue<T> { T Get(); }
delegate T Factory<T>();

class IntValue : IValue<int>
{
    int Get() { return 7; }
}

class Converter
{
    T Echo<T>(T value) where T : struct { return value; }
}

T Identity<T>(T value) { return value; }
int Make() { return 5; }

int main()
{
    int score = 0;
    List<int> list = new List<int>(1);
    list.Add(1); list.Add(2); list.Add(3); list.Add(4); list.Add(5);
    list.Insert(2, 9);
    list.Remove(2);
    list.RemoveAt(0);
    foreach (int value in list) score = score + value;
    if (list.Capacity() >= 8) score = score + 100;

    Queue<int> queue = new Queue<int>(1);
    queue.Enqueue(6); queue.Enqueue(7); queue.Enqueue(8);
    score = score + queue.Dequeue() + queue.Peek();
    foreach (int value in queue) score = score + value;

    Stack<int> stack = new Stack<int>(0);
    stack.Push(2); stack.Push(3); stack.Push(4);
    score = score + stack.Pop();
    foreach (int value in stack) score = score + value;

    HashSet<int> unique = new HashSet<int>(1);
    if (unique.Add(5)) score = score + 1;
    if (!unique.Add(5)) score = score + 1;
    if (unique.Add(6)) score = score + 1;
    if (unique.Remove(5)) score = score + 1;
    foreach (int value in unique) score = score + value;

    Dictionary<int, int> map = new Dictionary<int, int>(1);
    map.Put(1, 10); map.Put(2, 20); map.Put(1, 11);
    map.Remove(2); map.Put(3, 30);
    score = score + map.Get(1) + map.Count();
    foreach (int value in map) score = score + value;

    Range range = new Range(4);
    foreach (int value in range) score = score + value;
    IValue<int> contract = new IntValue();
    Factory<int> factory = Make;
    int current = contract.Get();
    Converter converter = new Converter();
    score = score + Identity(current) + factory() + converter.Echo(2);
    return score;
}
