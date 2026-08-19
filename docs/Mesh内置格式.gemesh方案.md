# Mesh 引擎内置格式（.gemesh）方案

> 状态：**代码实现中（2026-08-19 起）**
> 目标：为 Mesh 提供一种**引擎原生二进制格式**，一线加载（CPU 解析与 GPU 上传）最快、内存占用最小，
> 与现有 `MeshData → Mesh` 装配流水线零摩擦对接，并作为 OBJ / glTF 的**离线烘焙导出目标**。
> 前置依赖：`Mesh类拆分重构方案.md`（已完成，`MeshData` / `ModelLoader` 分派已就位）
>
> 已落地（实现进度，见 §12）：
> - `GEMeshLoader`（SerializeGEMesh / ParseGEMesh）chunk 托盘编解码
> - `ModelLoader::Parse` 注册 `.gemesh`，异步链路零改动复用
> - `ModelLoader::ConvertToGEMesh` 离线导出工具（.obj → .gemesh）

---

## 1. 背景与动机

当前 Mesh 的几何数据来源只有两条：

| 来源 | 产线 | 问题 |
|---|---|---|
| 内置几何体 | `GenerateBuiltinMeshData` → `Mesh::Create` | 只有 cube/plane/quad/sphere，规模固定 |
| 外部模型（OBJ / 未来 glTF） | `ParseModelData` 分派 → `Mesh::Create` / 异步 | 运行时解析、读文本、重复装配 |

对 OBJ 这类**装配型源格式**，每次加载都要在运行时经历 `文本解析 → 顶点装配 → 量化去重 → 切线计算 → 子网格拆分 → 法线/UV 归一`，这套 CPU 密集工作对「同一份资产反复加载」是纯粹的浪费。而 OBJ 又带不动引擎需要的一切（骨骼、动画、压缩精度、多 LOD……）。

`.gemesh` 要解决的是：**把「格式解析 × 几何预处理」从加载期搬到离线烘焙期**，让运行时只做「读文件 → 建缓冲」，把 Mesh 装配流水线压成一条几乎没有 CPU 处理的直线。

**设计原则**：`.gemesh` 是**装载格式而不是源格式**——它是 OBJ/glTF/FBX 的子集快照，不追求超越源格式的表达力；只追求「加载到可渲染状态」这一件事做到最快、最省、最确定。

---

## 2. 格式定位与取舍

### 2.1 它装载什么（负载内容）

直接对齐现有 `MeshData` 结构（`GE/include/GE/Render/Mesh.h`），保证反序列化后能无缝 `std::move` 进装配流水线：

| 数据 | 对应源 | 说明 |
|---|---|---|
| **顶点流** | `MeshData::vertices`（`Vertex`） | 位置 + 法线 + UV + 切线，已量化 + 已去重 |
| **索引流** | `MeshData::indices`（`uint32_t`） | 已保证三角形序列 |
| **子网格表** | `MeshData::subMeshes`（`SubMesh`） | firstVertex/vertexCount/firstIndex/indexCount + **materialName** |
| **材质属性** | `MeshData::materialData`（`MaterialData`） | PBR 参数 + 贴图路径引用 |
| **元信息** | （新增） | 格式版本、顶点布局描述、包围盒（AABB）、源资产来源等 |

> 顶点布局：v1 固定为现有 `Vertex`（48B，4 个 location），用 magic/版本号预留演进空间；v2 以后可开放自定义布局（压缩定点、额外 attribute）。

### 2.2 它**不**装载什么（边界外）

| 项 | 理由 |
|---|---|
| 骨骼 / 蒙皮权重 | 阶段 4 再做，届时扩格式（新增 vertex attribute 段），不影响 v1 布局 |
| 动画剪辑 | 属骨骼动画配套，跟随下一阶段 |
| LOD 多级网格 | 可在文件内用「多个子网格组」表达，v1 不做，留字段占位 |
| 贴图二进制 | 纹理仍走 `TextureManager` 按路径加载；`.gemesh` 只存**路径引用** |
| 材质完整定义 | 只存 `MaterialData` 属性快照，`MaterialManager` 据此构建真实 `Material` |

---

## 3. 文件布局设计

### 3.1 托盘布局（Chunk 风格，便于增量演进）

```
┌──────────────────────────────────────────┐
│  Magic    "GEMSH" (5) + version (u16=1)  │   8 字节头部
├──────────────────────────────────────────┤
│  ChunkTable: count(u32) + 每项{ id(u32)  │   按 id 索引，加载可只读需要的 chunk
│    , offset(u64) , size(u64) }           │
├──────────────────────────────────────────┤
│  Chunk: VERTICES   (id=1)                │   顶点流（原始字节，见 3.2）
│  Chunk: INDICES    (id=2)                │   索引流（u32 * n）
│  Chunk: SUBMESHES  (id=3)                │   子网格表（含 materialName 字符串池）
│  Chunk: MATERIALS  (id=4)                │   MaterialData 快照（含贴图路径字符串池）
│  Chunk: META       (id=5)                │   AABB + 顶点数/索引数 + 源资产字符串
└──────────────────────────────────────────┘
```

- **字符串池**：子网格 `materialName`、材质贴图路径 / 材质名等变长字符串集中放在各自 chunk 尾部的字符串池，
  表内用 `offset`（u32，池内字节偏移）+ `len`（u32）引用，去掉逐条 `std::string` 的堆分配与对齐浪费。
- **对齐**：每个 chunk 起点 4 字节对齐；顶点流内部按 `sizeof(Vertex)=48` 天然对齐。
- **字节序**：固定 little-endian（引擎目标平台）；`META` 写平台无关的 magic/endor。

### 3.2 顶点流编码（v1）

v1 直接落 `Vertex` 原始字节：

```
layout = { attr: Position(vec3 f32), Normal(vec3 f32), TexCoord(vec2 f32), Tangent(vec4 f32) }  // 48B
count = u32
data  = Vertex[count]      // 与 GPU 顶点缓冲的输入布局完全一致
```

**关键决策**：v1 顶点流**不压缩、不量化二次**——因为源解析时已经量化（`Vertex::Quantize`，1/10000 网格），
存储与 GPU 输入布局同构，可做**零拷贝直传**（`GetBufferInfo` 的 stride/offset 直接对用），省去解压重排。
压缩定点留 v2（配合顶点布局自描述，见 §8）。

所以整个几何数据**已按 GPU 可用的最终形态落盘**——读取 → `VulkanBuffer::Create`（staging）→ 上传，三步完成。

---

## 4. 加载路径（写入现有流水线）

接入点选择 `ModelLoader` 的分派器（`ParseModelData`），与 OBJ 完全对称：

```
MeshManager::Load("foo.gemesh")
  └─ ParseModelData
       ├─ parse 分派：头几个字节读 magic → 若非 GEMSH → 交给扩展名 fallback
       │   （或直接按 .gemesh 扩展名分派，二者取一，见 §4.2）
       ├─ ParseGEMeshData("foo.gemesh", out) : MeshData
       │    读 chunks → 反序列化 vertices/indices/subMeshes/materialData → std::move 进 out
       └─ 返回 MeshData → 走既有 Mesh::Create / 异步任务，装配链路零改动
```

### 4.1 为什么反序列化产物是 `MeshData`（而非直达 Mesh）

复用现有装配流水线 = 免费获得：
1. **切线计算一致性**：`ComputeTangents` 一条路径保证与 OBJ/内置几何体结果逐字节一致（烘焙期也可预计算切线，见 §6.1 可选）。
2. **异步上传**：`.gemesh` 同样享受 `AsyncUploadManager` 后台加载 + `IsReady()` 门控，与 OBJ 无差别。
3. **子网格材质构建**：`MeshManager::BuildSubMeshMaterials` 按 `materialName` 匹配 `materialData` 的既有逻辑直接生效。
4. **统一去重缓存**：`MeshManager` 按路径去重，无需为 `.gemesh` 单独开路径。

代价是读数稍多（多一层 `MeshData` 中转），但这一层本来就是 `std::move` 的零拷贝传递，可忽略。

### 4.2 Magic 嗅探 vs 扩展名分派

- **推荐：Magic 嗅探优先**。`ParseModelData` 目前按扩展名分派；对 `.gemesh` 同时保留扩展名直判。
  更稳的做法是：未知扩展名 → 读头部 magic 判断，属 GEMSH 则走解析器（防止改名/工具链问题）。
- 实现很轻：`ParseModelData` 顶部读 8 字节，`memcmp` 前 5 字节等于 `"GEMSH"` 即进入 `.gemesh` 分支，
  否则落到 RGB 分派。

### 4.2.1 场景序列化（零改动）

**结论：场景序列化不需要任何改动**，`.gemesh` 自动无缝接入。

原因：`SceneSerializer` 对网格只做一件事——存/取**文件路径字符串**：

```cpp
// 序列化（SceneSerializer.cpp:455-457）：只落盘路径
if (mc.MeshPtr && !mc.MeshPtr->GetFilePath().empty()) {
    meshNode["Mesh"] = mc.MeshPtr->GetFilePath();          // 存 "foo.gemesh"
}

// 反序列化（SceneSerializer.cpp:713-716）：按路径交给统一入口
if (meshNode["Mesh"]) {
    std::string meshPath = meshNode["Mesh"].as<std::string>("");
    mc.MeshPtr = Renderer::GetAssetManager().LoadMesh(meshPath);  // LoadMesh("foo.gemesh")
}
```

`LoadMesh` → `MeshManager::Load`（按路径去重）→ `ParseModelData` 分派。只要 `.gemesh`
完成了 §4 的 magic 嗅探接线，`LoadMesh("foo.gemesh")` 自然命中新解析器，**路径本身就是格式的
身份标识**，序列化代码对 `.gemesh` 和 `.obj` 一视同仁，无需 fork。

需要确认的**唯一约束**：`Mesh` 的 `GetFilePath()` 返回的必须是与加载时一致的 `.gemesh` 路径。
`ModelLoader` 的 `ParseModelData` 在解析时用入参 `filepath` 回填到 `Mesh::SetFilePath`（与 OBJ 相同逻辑），
因此烘焙出的 `.gemesh` 路径即为落盘值，天然一致，无需额外处理。

### 4.3 错误语义

- 文件不存在 / 无 GEMSH magic / chunk 表越界 / 字符串池越界 → 返回 `false`，沿用现有 `nullptr` 失败语义。
- **边界校验必做**：所有 chunk `offset+size` 必须落在文件范围内（防损坏文件导致的越界读），
  反序列化前集中校验一次。

---

## 5. 相对路径与资源根

`.gemesh` 内的贴图路径（`MaterialData::*Map`）与源 OBJ/glTF 一样是**相对文件所在目录**的路径，
加载时经 `AssetManager` 的 `ResolveResourcePath` 解析为绝对路径（与 OBJ 行为一致，`DocumentRoot`/资源根复用）。
不需要新机制。

> 注意：烘焙时要把源模型的 `map_Kd` 等相对路径**原样拷贝**进 `.gemesh`，不要烘焙成绝对路径，
> 否则移动资产目录后贴图路径即失效。

---

## 6. 工具链：离线烘焙导出器

### 6.1 导出器（Convers 工具，独立可执行或引擎子命令）

把 OBJ/glTF 批量转换为 `.gemesh`，同时**提前完成几何预处理**（这正是装载格式的意义）：

```
输入: foo.obj / foo.gltf       输出: foo.gemesh
步骤:
  1. 读源模型 → MeshData            （复用 ParseModelData / GLTFRawLoader）
  2. ComputeTangents(data)          （可选——若源已有切线可跳过，见下）
  3. （可选）顶点再量化 + 去重         （源未量化的场景）
  4. 写 chunk 表 + 各 chunk         （SerializeGEMeshData）
  5. 写 stdout / 日志：版本、AABB、顶点数、子网格数、输出体积
```

- **CLI 建议**：`gemesh -i in.obj -o out.gemesh [-q]`（`-q` 不二次量化、信任源数据），支持批量目录遍历。
- **确定性**：相同输入 → 相同字节输出（固定排序、固定量化），便于重复构建与 diff。
- 也可做**引擎内 `BuiltinMesh` 导出**：把内置几何体 `GenerateBuiltinMeshData` 的产物落成 `.gemesh` 作为资产。

### 6.2 反序列化接口

```
// 新增 GE/src/Render/GEMeshLoader.cpp（与 OBJLoader 对称）
bool ParseGEMeshData(const std::string &filepath, MeshData &out);
bool SerializeGEMeshData(const std::string &outPath, const MeshData &data,
                         const MetaInfo &meta, std::string *err);   // 导出器用
```

头文件挂在 `ModelLoader.h`（声明 `ParseGEMeshData`），序列化函数可放独立 `GEMeshWriter.h`（导出器/工具独享，不进运行时头）。

---

## 7. 接入路线（分阶段）

### 阶段 0（本方案落地）
- 设计定稿：chunk 布局、顶点布局 v1、magic/版本号
- 编写 `GEMeshWriter`（序列化）+ 首个导出器 CLI 骨架

### 阶段 1：运行时读取（可加载）
- `GEMeshLoader.cpp`：`ParseGEMeshData` + 边界校验 + 字符串池还原
- `ParseModelData` 加入 GEMSH magic 嗅探分支
- 用导出器烘焙一个 OBJ → `.gemesh`，运行时替换加载，验证与 OBJ 加载结果**逐顶点一致**

### 阶段 2：接入统一入口 + 异步
- 确认 `.gemesh` 走 `MeshManager::Load` / `AssetManager::LoadMesh` 自动生效（异步上传 / IsReady / 材质构建不动）
- 编辑器资源面板读取 `.gemesh` 元信息（AABB / 顶点数）展示；场景序列化存 `.gemesh` 路径

### 阶段 3（可选）：格式演进
- 顶点布局自描述 + 压缩定点（节省显存/带宽）
- 骨骼槽位 / 动画剪辑 chunk
- LOD 子网格组

---

## 8. 压缩与显存考量（v2 展望）

- **v1 用 f32 直存**：图解析期量化 + 紧凑打包，OBJ 已丢精度、体积已收敛，v1 不追求二次压缩。
- **v2 定点压缩顶点**：Position 量化到 `int16`（比例因子存 META），Normal/Tangent 用 `Snorm8`，
  UV 用 `Snorm16` —— 需 `Vertex` 布局自描述（chunk 加 `layoutDesc` 字段），并与 GLTF `KHR_mesh_quantization` 设计对齐。
- **索引**：v1 恒 `uint32_t`；v2 可选 `uint16`（vertexCount < 65536 时）进一步省显存。

---

## 9. 风险与对策

| 风险 | 对策 |
|---|---|
| 与现有装配流水线脱节，引入第二套逻辑 | 反序列化产物就是 `MeshData`，走 `Mesh::Create`/异步既有路径，一条链不复制 |
| 加载后结果与 OBJ 不一致 | 阶段 1 强制「同一模型 OBJ 与 .gemesh 加载结果逐字节比对」的验收项 |
| 字符串池 / chunk 越界导致崩溃 | 反序列化前集中边界校验（offset+size 必须在文件内），越界一律返回 false |
| 贴图路径烘焙成绝对路径，迁移资产后失效 | 导出器强制保留相对路径；加载经 `ResolveResourcePath` 还原（§5） |
| 私定二进制随需求演进无法兼容 | 头部版本号 + magic + chunk 表（未知 chunk 跳过），保证向后可读 |
| 现阶段骨骼/动画仍需源格式 | 明确阶段边界：`.gemesh` v1 只装几何，骨骼动画阶段再扩 chunk，不阻塞本期 |

---

## 10. 验收标准

- 用导出器把 `cube.obj` 烘焙为 `cube.gemesh`；`Renderer::GetAssetManager().LoadMesh("cube.gemesh")`
  与 `LoadMesh("cube.obj")` 产出**顶点/索引/子网格/材质数据完全一致**（自动化 diff 脚本）
- `.gemesh` 加载火焰：`ParseGEMeshData` 无文本解析、无二次量化、无切线计算（或仅一次轻量走查），
  加载耗时与文件体积线性，显著低于同模型 OBJ 加载
- 异步加载 / `IsReady()` 门控 / 子网格默认材质构建 / 编辑器网格统计 与 OBJ 路径行为一致
- 损坏文件（截断 / 篡改 chunk 表）返回 `false` 而非崩溃
- 向后兼容：用 v2 写出的（含新 chunk）文件，v1 加载器可安全忽略未知 chunk 正常加载

---

## 11. 与现有方案的衔接

- `Mesh类拆分重构方案.md`：`.gemesh` 反序列化输出 `MeshData`，正是拆分后 `Mesh::Create` / 异步注入的天然输入，直接叠加。
- `glTF导入与骨骼动画实施方案.md`：glTF 加载器（`GLTFRawLoader`）可作为导出器输入端之一；骨骼 chunk 与 glTF 蒙皮字段对齐。
- `Model作为加载唯一入口重构方案.md`：`.gemesh` 遵循统一 `AssetManager::LoadMesh` 入口，不另开加载链路。
- `异步上传方案.md`：`.gemesh` 异步路径复用 `AsyncUploadManager`，无需改动。

---

## 12. 实现进度

### 已落地（2026-08-19）

| 项 | 文件 | 说明 |
|---|---|---|
| 编解码 | `GE/src/Render/GEMeshLoader.cpp` / `GEMeshLoader.h` | `ParseGEMesh`（.gemesh→MeshData）+ `SerializeGEMesh`（MeshData→.gemesh）。chunk 托盘：VERTICES/INDICES/SUBMESHES/MATERIALS/META，字符串池 + 逐处边界校验 |
| 接入分派 | `ModelLoader.cpp` | `ModelLoader::Parse` 增加 `ext==".gemesh"` → `ParseGEMesh`；`MeshManager::Load` 异步链路零改动 |
| 导出工具 | `ModelLoader.cpp` | `ModelLoader::ConvertToGEMesh(src, out)`：Parse → ComputeTangents → 包围盒 → SerializeGEMesh |
| CLI 导出器 | `tools/gemesh/gemesh_main.cpp` + 根 `CMakeLists.txt` | `gemesh <src.obj> [out.gemesh]` 独立可执行，链接 GE 复用 ConvertToGEMesh，纯 CPU 无窗口 |

### 待办

- **构建验证**（用户执行）：`build.bat` 全量；产出 `cube.gemesh` 后 `LoadMesh` 与 `LoadMesh(cube.obj)` 结果比对
- **编辑器 UI**：`SceneHierarchyPanel.cpp` 的「加载模型文件」文件对话框过滤器加 `*.gemesh` 项
- **Magic 嗅探**（可选加固）：`ModelLoader::Parse` 当前按扩展名分派；需更强健可改为头部 magic 嗅探

> 格式细节以 `GEMeshLoader.cpp` 头常量与 `SerializeGEMesh`/`ParseGEMesh` 实现为准（v1 固定 little-endian，
> `Vertex` 48B 直落，字符串池 -1=空）。
>
> 构建后 CLI 位于 `bin/gemesh.exe`，用法：`gemesh models/foo.obj` → 生成 `models/foo.gemesh`。
