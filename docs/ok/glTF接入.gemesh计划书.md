# glTF 接入 .gemesh 计划书

> 状态：**方案设计（待评审）**
> 目标：厘清 glTF 未来导入时与 `.gemesh` 的对接关系——**`.gemesh` 按什么粒度烘焙、node 层级与变换归属谁、glTF 导入器与 `.gemesh` 怎么分工**。
> 前置：`Mesh内置格式.gemesh方案.md`（已实现运行时读取 + CLI 导出器）
> 关联：`Mesh类拆分重构方案.md`（MeshData / ModelLoader 分派架构已就位）

---

## 1. 要回答的问题

> 「我未来加入 glTF 的时候，是不是把他的每个 node 都生成一份 gemesh？」

**直接回答：不是。** `.gemesh` 按**网格（mesh）**粒度烘焙，**不按 node**。node 的层级/变换/骨骼不属于几何，`.gemesh`（本质是 `MeshData` 的序列化，只有顶点/索引/子网格/材质）承载不了，也不该承载。

本计划书说明这个粒度决策的完整依据、glTF 导入器与 `.gemesh` 的分工、以及落地路线。

---

## 2. glTF 的结构：node 和 mesh 是两回事

```
scene
 └─ node A（变换矩阵 + 骨骼）      ← 层级/变换/骨骼在 node
     └─ mesh M1
         ├─ primitive 0            ← 一个子网格（顶点流 + 材质索引）
         ├─ primitive 1
 └─ node B
     └─ mesh M1                    ← 同一 mesh 可被多个 node 复用
 ```

**关键事实**：node 和 mesh **不是一一对应**。以当前仓库资产 `assets/models/adamHead/adamHead.gltf` 实测：

| 资产 | nodes | meshes | 说明 |
|---|---|---|---|
| adamHead.gltf | 80 | 76 | 含骨骼 + 大量单 primitive 子网格 + blend shape（动画） |

node 数目 ≠ mesh 数目，因为它们表达不同信息：
- **node**：场景图节点，携带**局部变换 / 父子层级 / 骨骼关节（joint）**
- **mesh**：几何资源，携带**顶点 / 索引 / primitive（= 子网格）/ 材质引用**

`.gemesh` 序列化的是 `MeshData` = **几何资源**。所以粒度必然落在 mesh 层。

---

## 3. 核心决策：`.gemesh` 粒度 = 每唯一 mesh

### 3.1 粒度规则

| 层 | 是否生成 `.gemesh` | 理由 |
|---|---|---|
| **每个唯一 mesh 一次** | **是** | `.gemesh` 承载该 mesh 的几何：其所有 primitive 序列化为若干子网格（对应 `SubMesh`） |
| 每个 node | **否** | node 无几何，只有变换/层级/骨骼；多 node 复用同 mesh 时应共享同一 `.gemesh` |
| primitive | 不单独成文件 | 同一 mesh 的多个 primitive 就是 `.gemesh` 内的多个子网格，共用顶点/索引缓冲 |

### 3.2 为什么「每 node 一份」是错的

1. **重复**：多个 node 引用同一 mesh → 生成多份相同 `.gemesh`，浪费磁盘与加载。先按 mesh 去重，再让多 node 共享——正好利用 `MeshManager` 的按路径去重缓存。
2. **语义错位**：node 变换塞进 `.gemesh` 会让「同一个几何在不同位置出现两份不同数据」，破坏资源的可复用性。
3. **违背 `MeshData` 模型**：`.gemesh` 反序列化进 `MeshData`（无 transform 字段）。node 信息无处安放。

### 3.3 直接结论

```
glTF 文件里有 N 个唯一 mesh → 生成 ≤ N 份 .gemesh（每 mesh 一份，多 node 复用去重）
```

---

## 4. node / transform / 骨骼 归属谁

`.gemesh` 是**纯几何网格**，以下信息**不进 `.gemesh`**：

| 信息 | 归属 | 说明 |
|---|---|---|
| node 局部变换 / 平移旋转缩放 | **glTF 场景图导入器**（建实体时填 `TransformComponent`） | 导入器读 node 矩阵填到实体 |
| node 父子层级 | **场景图导入器**（`Scene::SetParent`） | `.gemesh` 无层级概念 |
| 骨骼关节（joint）+ 蒙皮权重 | **骨骼动画阶段**（future），不阻塞 `.gemesh` v1 | 见 §8 风险 |
| blend shape（形态键） | 动画，`.gemesh` v1 不承载 | adamHead 有大量 blend shape |

**分工原则**：`.gemesh` 专注「把它当资源能立即渲染」；node 的"摆放/层级/动起来"是场景图导入器的职责，两者解耦。

---

## 5. 两种导入语义（决定 glTF 在引擎里怎么用）

### 情形 A：glTF 当「网格资源」加载（.gemesh 直通）

```
AssetManager::LoadMesh("model.gltf")  或  LoadMesh("model.gemesh")
  → 得到一个 Mesh（多子网格 + 材质），自己摆 transform
```

- `.gemesh` 完全胜任：反序列化出 `MeshData` → `Mesh::Create`。
- glTF 若已被烘焙成 `.gemesh`，`LoadMesh(model.gemesh)` 直接走快的二进制路径。
- 这种情形**不需要** node 层级——用户只要「这个 model 长什么样」。

### 情形 B：glTF 当「场景文件」导入（建实体树）

```
glTF 场景图导入器
  → 每 node 建一个实体
  → 有 mesh 的 node：挂 MeshRendererComponent（引用对应 .gemesh）+ 填 TransformComponent
  → 父子关系：SetParent
```

- 这是「一文件 → 多实体层级」的完整导入。
- **node 变换必须在导入器里落地到实体**，`.gemesh` 不参与。
- 同一 `.gemesh` 被多个 node 的组件共享（MeshManager 去重）。

---

## 6. 烘焙管线（glTF / .glb → .gemesh）

复用现有 `ModelLoader::ConvertToGEMesh` 形态，但 glTF 需先经解析器产出对应 mesh 的 `MeshData`：

```
glTF / .glb 文件
  → glTF 解析器（GLTFRawLoader，逐 mesh 产出 MeshData）
      ├─ mesh[0] → ConvertToGEMesh → mesh0.gemesh
      ├─ mesh[1] → ConvertToGEMesh → mesh1.gemesh
      └─ ...
  （也可导出器整体遍历：每唯一 mesh 落一份 .gemesh，命名可沿用 glTF 内 mesh name 或索引）
```

**注意**：`.gemesh` 单文件 = 单 mesh（多子网格）。一个 glTF 文件多 mesh → 多份 `.gemesh`，这是与 OBJ 的差别（OBJ 通常一个形状集 → 一份）。

---

## 7. 落地路线

### 阶段 1：glTF 单 mesh 烘焙（最小可用）
- `GLTFRawLoader` 能读 glTF → 产出一个 mesh 的 `MeshData`（含顶点/索引/子网格/材质）
- `ModelLoader::ConvertToGEMesh` 支持传入任意 `MeshData`（已支持，因为接口就是 `MeshData→SerializeGEMesh`）
- CLI 导出器支持 `gemesh in.gltf -o out.gemesh`（取第一个/指定 mesh）——`ModelLoader::Parse` 需加 `.gltf` 分支（当前只认 `.obj`/`.gemesh`）

### 阶段 2：多 mesh + node 复用
- glTF 解析器遍历所有 mesh，每唯一 mesh 烘焙一份 `.gemesh`
- `ModelLoader::Parse` 支持 `.gltf`/`.glb`：加载时若目标是单 mesh 资源，直接重建该 mesh 的 `MeshData`
- node 复用去重：同一 mesh 多处引用 → 只一份 `.gemesh` + MeshManager 缓存

### 阶段 3：场景图导入（与 .gemesh 解耦）
- glTF 场景图导入器（若开发）建实体树，node 变换落 `TransformComponent`，mesh 引用走 `.gemesh`
- `.gemesh` 自身**不加** node/transform 字段，保持纯净

> glTF 场景图导入本身是否开发、是否为优先项，由单独方案决定；`.gemesh` 只保证「几何资源层」就绪。

---

## 8. 边界与风险

| 项 | 处理 |
|---|---|
| **多 primitive mesh** | primitive → 子网格，同 `.gemesh` 共享顶点/索引缓冲（与 OBJ 多子网格一致） |
| **单 primitive mesh 极多**（adamHead 76 mesh） | 每 mesh 一份 `.gemesh` 会导致大量小文件。可接受（加载按需）；若要合并再考虑「整文件一个 `.gemesh` + 多子网格」的变体，但会牺牲复用 |
| **骨骼/蒙皮** | `.gemesh` v1 不承载。蒙皮肤色需要 `Vertex` 加关节/权重字段并扩格式（阶段 4），届时 `Vertex` 布局变化会改 `.gemesh` 顶点流编码（`static_assert(sizeof(Vertex)==48)` 需同步调整） |
| **blend shape（adamHead 眼球）** | 动画用，`.gemesh` v1 不支持。若需静态烘焙某形态，导出时用该形态的顶点流即可 |
| **node 变换** | 绝不进 `.gemesh`；否则破坏资源复用 + 违背 MeshData 模型 |
| **材质差异** | glTF 材质（PBR metallic-roughness）与 OBJ 的 `MaterialData` 字段有出入；`.gemesh` 的 MATERIALS chunk 承载 `MaterialData` 即 OBJ 语义，glTF 导入要么映射到现有字段，要么扩 MATERIALS chunk |
| **`.gemesh` 顶点流对更大模型** | v1 直接落 `Vertex`(48B)。骨骼阶段若加字段需重编码；考虑预留 layout desc（见 .gemesh 方案 §8 v2） |

---

## 9. 验收标准

- adamHead 类资产：`nodes=80, meshes=76` → 烘焙产 **≤76 份** `.gemesh`（每唯一 mesh 一份），多 node 复用同 mesh 不重复生成
- `LoadMesh(model.gemesh)` 与 `LoadMesh(model.gltf)`（资源语义）产出几何一致
- node 变换 / 层级在场景图导入路径上正确落地到实体，`.gemesh` 文件内**无任何 transform 字段**
- 单 primitive mesh 加载无额外内存/文件开销，MeshManager 按路径去重生效

---

## 10. 与现有方案的衔接

- `Mesh内置格式.gemesh方案.md`：本计划书是它的 glTF 接入扩展，粒度与 node 归属组件明确。
- `Mesh类拆分重构方案.md`：`.gemesh` 反序列化输出 `MeshData`，glTF 解析器（GLTFRawLoader）也是输出 `MeshData`，两者在 `ModelLoader::Parse` 分派下同构。
- 原 `glTF阶段2场景图导入` 方案书已移除；若未来恢复，node 变换落实体、mesh 引用 `.gemesh` 的分工与本计划书一致。
