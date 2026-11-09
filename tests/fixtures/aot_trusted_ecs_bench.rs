module ecs_bench;

struct Transform {
    float x;
    float y;
    float z;
}

struct Velocity {
    float vx;
    float vy;
    float vz;
}

struct GridPos {
    int gx;
    int gy;
}

struct ActiveTag {
    int flag;
}

enum EntityState {
    Idle = 0,
    Moving = 2
}

struct StructWithEnum {
    EntityState state;
    int ticks;
}

struct StructWithStatic {
    int x;
    int y;
    public static int sharedBias = 10;
}

class EcsSystems {
    public static void stepMovement(float dt) {
        foreach (ref var t, in var v in Query<Write<Transform>, Read<Velocity>>()) {
            t.x = t.x + v.vx * dt;
            t.y = t.y + v.vy * dt;
            t.z = t.z + v.vz * dt;
        }
    }

    public static void stepGrid(int dx, int dy) {
        foreach (ref var g in Query<Write<GridPos>>()) {
            g.gx = g.gx + dx;
            g.gy = g.gy + dy;
        }
    }

    public static int stepFilteredGrid(int dx, int dy) {
        int visited = 0;
        foreach (int entityId, ref var g in Query<Write<GridPos>, With<ActiveTag>>()) {
            g.gx = g.gx + dx;
            g.gy = g.gy + dy;
            visited = visited + entityId;
        }
        return visited;
    }

    public static int verifyEdgeCases() {
        StructWithEnum swe = new StructWithEnum();
        swe.state = EntityState.Moving;
        swe.ticks = 32;

        StructWithStatic sws = new StructWithStatic();
        sws.x = 5;
        sws.y = 6;

        int[] arr = new int[1];
        arr[0] = 10;
        if (arr[0] == 10) {
            arr = new int[3];
        }
        arr[2] = 40;

        return swe.ticks + sws.x + sws.y + StructWithStatic.sharedBias + arr[2];
    }
}
