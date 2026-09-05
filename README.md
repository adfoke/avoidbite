# 旱厕蹲坑算法

用并发编程模拟「占坑蹲着 + 蚊子袭扰加速完成」的过程：主线程是蹲坑者，蚊子是工作线程；叮咬推动共享进度，进度到 100% 即离坑。

## 原理概览

| 角色 | 实现 | 行为 |
|------|------|------|
| 坑位 `Pit` | 共享状态 + 锁 / 条件变量 | 保存进度、叮咬次数、完成标志 |
| 蹲坑者 | 主线程 `squat` | 每 80ms 自身 +1 进度（慢） |
| 蚊子 | `pthread` 工作线程 | 随机间隔叮咬，按公式加进度（快） |
| UI | 进度条 | 叮咬或自身推进后刷新；持 `ui_lock`，丢弃过期快照 |

核心直觉：**袭扰越猛，完成越快**；但叮咬收益会递减，蚊子过密还会互相干扰，避免「线程越多越好」的线性幻觉。

```mermaid
flowchart LR
    subgraph 隐喻
        A[蹲坑者] -->|占坑| P[(坑位进度)]
        M1[蚊子] -->|叮咬| P
        M2[蚊子] -->|叮咬| P
        Mn[蚊子…] -->|叮咬| P
        P -->|进度 ≥ 100%| D[离坑]
    end
```

## 整体架构

```mermaid
flowchart TB
    MAIN[main] --> INIT[初始化 Pit<br/>mutex / cond / ui_lock]
    INIT --> SPAWN{蚊子数量 N > 0?}
    SPAWN -->|是| CREATE[创建 N 个蚊子线程]
    SPAWN -->|否| SQUAT
    CREATE --> SQUAT[主线程 squat]
    SQUAT --> DONE[progress ≥ 100<br/>broadcast 唤醒蚊子]
    DONE --> JOIN[join 全部蚊子]
    JOIN --> REPORT[输出叮咬数与耗时]
    REPORT --> EXIT[离坑 / 退出]

    CREATE -.-> MOSQ[蚊子线程循环]
    MOSQ -.-> DONE
```

## 主流程

```mermaid
sequenceDiagram
    participant Main as 主线程 squat
    participant Pit as Pit 共享状态
    participant M as 蚊子线程们
    participant UI as 进度条

    Main->>Pit: 初始化 progress=0
    Main->>M: pthread_create × N
    loop 直至 done
        Main->>Main: sleep 80ms
        Main->>Pit: lock → progress += 1
        alt progress ≥ 100
            Main->>Pit: done=1, broadcast
        end
        Main->>Pit: 快照后 unlock
        Main->>UI: draw_bar
    end

    loop 直至 done
        M->>Pit: timedwait 随机 30–150ms
        alt 被 broadcast 叫醒且已 done
            M-->>M: 退出
        else 超时且未完成
            M->>Pit: progress += bite_delta()
            M->>Pit: bites++
            opt progress ≥ 100
                M->>Pit: done=1, broadcast
            end
            M->>UI: draw_bar
        end
    end

    Main->>M: join
    Main->>Main: 打印耗时并退出
```

## 蹲坑者与蚊子状态机

```mermaid
stateDiagram-v2
    [*] --> Occupied: 占据坑位

    state Occupied {
        [*] --> Squatting
        Squatting --> Squatting: 自身 +1 / tick
        Squatting --> Squatting: 被叮咬 +δ
        Squatting --> Done: progress ≥ 100
    }

    state MosquitoLoop {
        [*] --> Waiting
        Waiting --> Bite: timedwait 超时
        Bite --> Waiting: 更新进度并刷新 UI
        Waiting --> Exit: done == 1
        Bite --> Exit: 本次叮咬使完成
    }

    Done --> [*]: 离坑
    Exit --> [*]
```

## 叮咬收益公式

每次叮咬增加的进度不是常数，而是：

\[
\delta = \max\!\Bigl(1,\; 4 - \lfloor bites / 10 \rfloor - \bigl\lceil \max(0,\; n - 8) / 4 \bigr\rceil\Bigr)
\]

其中：

- `4`：基础叮咬收益（`BITE_BASE`）
- `⌊bites / 10⌋`：累计叮咬越多，单次收益越弱（递减）
- `n > 8`：蚊子过密时互相干扰；超额按 `⌈(n-8)/4⌉` 扣减（`INTERFERE_THRESH`）
- 下限为 `1`（`BITE_FLOOR`），不会叮了没效果

实现上用整数 `(overcrowded + 3) / 4` 计算向上取整，保证第 9 只起就生效。

```mermaid
flowchart TD
    START[蚊子叮一口] --> D1["delta = 4 - bites/10"]
    D1 --> CLAMP1{delta < 1?}
    CLAMP1 -->|是| F1[delta = 1]
    CLAMP1 -->|否| NEXT
    F1 --> NEXT{n_mosq > 8?}
    NEXT -->|是| SUB["delta -= ceil n-8 / 4"]
    NEXT -->|否| APPLY
    SUB --> CLAMP2{delta < 1?}
    CLAMP2 -->|是| F2[delta = 1]
    CLAMP2 -->|否| APPLY
    F2 --> APPLY[progress += delta<br/>bites++]
```

因此：

- **0 只蚊子**：只靠自身每 tick +1，最慢（约 8s）
- **适量蚊子**：叮咬主导加速，明显变快
- **过多蚊子**：单次收益被压到下限，加速出现边际递减

## 收益曲线

### 理论：单次叮咬收益 δ

随累计叮咬衰减（蚊子数 ≤ 8，无干扰扣减）：

```mermaid
xychart-beta
    title δ 随累计叮咬数衰减
    x-axis ["0~9 口", "10~19 口", "20~29 口", "≥30 口"]
    y-axis "单次收益 δ" 0 --> 5
    bar [4, 3, 2, 1]
```

蚊子过密时的前期收益（累计 < 10 口），按 `⌈(n-8)/4⌉` 扣减：

```mermaid
xychart-beta
    title 前期 δ 随蚊子密度被压缩
    x-axis ["≤8 只", "9~12 只", "13~16 只", "≥17 只"]
    y-axis "单次收益 δ" 0 --> 5
    bar [4, 3, 2, 1]
```

### 实测：蚊子数 → 平均离坑耗时

`./build/avoidbite N` 各连跑取均值（macOS arm64；N=0 跑 3 次，其余 5 次；耗时为程序自报）：

```mermaid
xychart-beta
    title 蚊子数 vs 平均离坑耗时
    x-axis [0, 1, 2, 3, 5, 8, 12, 16, 24, 32, 48, 64]
    y-axis "耗时 (s)" 0 --> 9
    bar [8.35, 2.15, 1.26, 0.94, 0.67, 0.50, 0.59, 0.58, 0.45, 0.33, 0.25, 0.25]
    line [8.35, 2.15, 1.26, 0.94, 0.67, 0.50, 0.59, 0.58, 0.45, 0.33, 0.25, 0.25]
```

| 蚊子数 | 平均耗时 (s) | 平均总叮咬 |
|-------:|------------:|----------:|
| 0  | 8.35 | 0  |
| 1  | 2.15 | 23 |
| 2  | 1.26 | 28 |
| 3  | 0.94 | 30 |
| 5  | 0.67 | 33 |
| 8  | 0.50 | 35 |
| 12 | 0.59 | 64 |
| 16 | 0.58 | 84 |
| 24 | 0.45 | 96 |
| 32 | 0.33 | 97 |
| 48 | 0.25 | 98 |
| 64 | 0.25 | 98 |

三个读数：

1. **0 → 8 只斜率最陡**：8.35s → 0.50s，约 94% 的收益在前 8 只就拿到了。
2. **8~16 只是平台区，还会抖动**（8 只 0.50s 反而比 12 只 0.59s 快）：δ 压到底 + 锁竞争加剧，总叮咬数翻倍却换不来时间——「线程越多越好」的线性幻觉在这段被实测打脸。
3. **≥48 只贴地 ~0.25s**：进度早已由叮咬推动（总叮咬逼近 100 × δ=1 的下限），只受 `pthread_cond_timedwait` 最短间隔和互斥串行化约束，存在硬地板。

## 同步与 UI 原理

```mermaid
flowchart LR
    subgraph 数据面
        L[lock] --> S[progress / bites / done]
        CV[cond cv] -->|broadcast| W[蚊子 timedwait]
        S --> CV
    end

    subgraph 显示面
        UL[ui_lock] --> BAR[进度条]
        SNAP[解锁后的快照] --> UL
    end

    S -->|拷贝快照后解锁| SNAP
```

要点：

1. **`lock`**：保护 `progress` / `bites` / `done`；叮咬与自身推进都在锁内修改。
2. **`cv`**：完成时 `broadcast`；蚊子用 `pthread_cond_timedwait`，既可定时叮咬，也可被提前叫醒退出。
3. **先改状态，再解锁，再画 UI**：避免持锁做 `printf` 堵住所有蚊子。
4. **`ui_lock` + 单调刷新**：并发刷新时丢弃更旧的快照，防止进度条回跳。

## 关键参数

| 宏 | 默认值 | 含义 |
|----|--------|------|
| `PROGRESS_GOAL` | 100 | 完成阈值 |
| `NUM_MOSQUITOES` | 5 | 默认蚊子数 |
| `BASE_TICK_MS` | 80 | 蹲坑者推进间隔 |
| `BASE_PROGRESS` | 1 | 自身每 tick 增量 |
| `BITE_BASE` | 4 | 叮咬基础收益 |
| `BITE_FLOOR` | 1 | 叮咬收益下限 |
| `INTERFERE_THRESH` | 8 | 开始过密干扰的蚊子数 |
| `MAX_MOSQUITOES` | 64 | 允许的最大蚊子数 |

## 编译与运行

```bash
cmake -S . -B build
cmake --build build

./build/avoidbite        # 默认 5 只蚊子
./build/avoidbite 0      # 无蚊子，最慢
./build/avoidbite 20     # 过密，干扰生效
```

```bash
cmake --build build --target clean   # 清理
rm -rf build                          # 彻底删除构建目录
```
