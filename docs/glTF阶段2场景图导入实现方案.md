# glTF 阶段 2 场景图导入实现方案

> 状态：**待实施**
> 前置：`docs/glTF导入与骨骼动画实施方案.md` 阶段 1（实体父子系统）已落地
> 目标：一个 .gltf/.glb → 一棵实体树（每个带 mesh 的 node 一个实体、局部坐标、父子层级复用阶段 1），
>       材质走既有 MaterialData / MaterialManager / TextureManager 通道，GLB 内嵌贴图从内存加载。

---

## 1. 背景与动机

阶段 1 已把地基打好：`TransformComponent.parent` + Scene 反向索引 + `SetParent` 单一入口 + 每帧 DFS
世界矩阵缓存 + UUID 序列化两遍反序列化。阶段 1 的层级面板已经能树形显示、拖拽重设父级。

阶段 2 要用这套地基把 **glTF 场景容器**真正导入：OBJ 是一文件一几何，glTF 是一文件多 mesh + node 层级
+ 每 node 局部变换 + 可选骨骼/动画。当前引擎的「一文件 = 一个 Mesh = 一个实体」契约放不下 glTF——
导入后的产物必须是一棵实体树，且每个带 mesh 的 node 是独立实体（轮子可单独转、部件可交互）。

**关键约束**：实体树必须在主线程创建——Scene 的 EnTT registry 非线程安全；而最耗时的 glTF 解析/贴图解码
必须放后台。因此本阶段采用**两阶段异步**：后台解析+上传，主线程回收时建实体树。

**顶层决策（已拍板）**：

| 维度 | 决策 |
|---|---|
| 导入执行 | **两阶段异步**：后台解析 + GPU 上传，主线程 `AsyncUploadManager::Poll()` 回收时建实体树（复用现成后台线程，不新起线程） |
| 分层 | 资源层 `GLTFRawLoader`（纯 CPU，不碰 Scene） + 场景层 `GLTFImporter`（编排 + 建实体树） |
| 网格复用 | 多个 node 引用同一 glTF mesh → 复用同一 `Mesh*`（key=`path#mesh{网格索引}`） |
| 序列化 | 实体树复用阶段 1 序列化字段，网格路径存 `path#mesh{n}` key，保存→重载零改动 |
| doubleSided / alphaMask / normalScale | **仅记录字段**，渲染端生效后续再做（已拍板） |
| 骨骼/关节/形态目标/动画 | 本阶段忽略（阶段 3/4），带动画角色以绑定姿势显示 |

---

## 2. 为什么不能直接复用 `Mesh::LoadFromFileAsync`

`LoadFromFileAsync` 的契约是「一次调用 = 后台产出一个 `Mesh*`」（1 对 1）。glTF 需要「一次导入 = N 个 Mesh
+ 一棵实体树」（1 对 N）。硬套会有四个摩擦点：

1. 一个文件要解析 N 遍（每个 `#mesh{n}` 键调一次，每次都全量解析整个 .glb）
2. `onInstalled` 按单个 Mesh 触发，没有「全部就绪后再建树」的完成协调点
3. `onInstalled`/`BuildSubMeshMaterials` 绑定 OBJ 材质构建逻辑，glTF 材质有其专属分支
4. 它不返回 node 层级信息，实体树信息仍要另解析

**复用的是机制**：`AsyncUploadManager` 的 `decode / upload / finalize` 三阶段模型（后台线程 + 每帧主线程
Poll 回收）就是本方案异步导入的骨架——把**整个 glTF 文件打包成单个任务**，后台解析一次、给 N 张网格
都建好 GPU 缓冲，主线程回收时统一组装 + 建实体树。

---

## 3. 总体架构

```
┌──────────────────── 资源层（产出可复用资源，不碰 Scene）───────────────────────────┐
│  GLTFRawLoader  (GE/src/Render/GLTFRawLoader.cpp)                                  │
│    · 唯一 include tiny_gltf.h 的 TU（声明用；实现由 tiny_gltf.cc 提供）              │
│    · ParseGLTF()          .gltf/.glb → GLTFRawFile（nodes/meshes/images/materials）│
│                          纯 CPU：解析 + 顶点装配 + 贴图像素解码，无 GPU 依赖        │
│    · BuildGLTFMesh()      一个 glTF mesh → 一个 Mesh（顶点装配 + GPU 上传，需 device）│
│    · BuildGLTFMaterial()  一个 glTF 材质 → 一个 Material（含内嵌贴图、linear 纯色，   │
│                          经 TextureManager 触发贴图加载上传，需 texMgr）            │
│    · AssignGLTFSubMeshMaterials()  把材质挂到 Mesh 各子网格 defaultMaterial          │
│    注：分层按「是否碰 Scene」划分，并非不碰 GPU——建 Mesh/Material 必然上 GPU。       │
└───────────────────────────────────────────────────────────────────────────────────┘
                            ▲                            ▲
                ImportAsync(decode/upload 调用)      MeshManager::Load("#mesh{n}") 调用
┌────────────────────────── 场景层 ─────────────────────────────────────────────────┐
│  GLTFImporter  (GE/src/Render/GLTFImporter.cpp)                                     │
│    · ImportAsync(Scene&, path, cb)  提交 AsyncUploadManager 任务                   │
│        decode（后台）：ParseGLTF + 逐 mesh 切线                                     │
│        upload（后台）：逐 mesh 建 GPU 顶点/索引缓冲 + 录拷贝                        │
│        finalize（主线程 Poll）：建 Mesh → 注册 MeshManager → 建材质 → 递归建实体树   │
│    · AbortAllPending()   场景切换时作废在途导入（防 use-after-free）                 │
└────────────────────────────────────────────────────────────────────────────────────┘
```

两条路径入口：
1. **编辑器导入**（主路径）：按钮选 .gltf/.glb → `GLTFImporter::ImportAsync` → 实体树出现在场景
2. **序列化回读**（兜底）：重开场景遇到 `path#mesh{n}` 网格键 → `MeshManager::Load` 同步解析 + 创建注册

---

## 4. 新增文件与 API

### 4.1 `GE/include/GE/Render/GLTFRawLoader.h` + `GE/src/Render/GLTFRawLoader.cpp`

**中间结构（tinygltf 类型不外泄）**：

```cpp
struct GLTFNodeInfo {
    std::string name;
    int mesh = -1;                  // 绑定的 glTF mesh 索引（-1 = 无网格，纯 transform 节点）
    std::vector<int> children;      // 子节点索引（指向本文件的 node 下标）
    bool   hasMatrix = false;       // glTF node 二选一：matrix 或 TRS
    glm::mat4 matrix = glm::mat4(1.0f);
    glm::vec3 translation{0.0f};    // TRS 形式（rotation 为四元数，对齐 TransformComponent）
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

struct GLTFRawMesh {
    std::vector<Vertex> vertices;           // 已量化 + 去重
    std::vector<uint32_t> indices;
    std::vector<SubMesh> subMeshes;         // 每个 primitive 一个子网格，materialName 带材质索引标记
    std::vector<int> primitiveMaterials;    // 每子网格的 glTF 材质索引（-1 = 无）
};

struct GLTFRawImage {
    std::string uri;                        // 外部图片绝对路径（内嵌/数据URI 时为空）
    bool embedded = false;                  // 内嵌：GLB bufferView 或 data URI
    std::vector<uint8_t> pixels;            // 内嵌图已解码 RGBA8（tinygltf 默认 LoadImageData 解好）
    uint32_t width = 0, height = 0;
};

struct GLTFRawMaterial {
    std::string name;
    glm::vec3 baseColorFactor{1.0f}; float baseColorAlpha = 1.0f;
    int baseColorTexture = -1;              // 引用 GLTFRawFile::images 下标
    float metallicFactor = 1.0f, roughnessFactor = 1.0f;
    int metallicRoughnessTexture = -1;      // 合并贴图：B=metallic, G=roughness
    int normalTexture = -1; float normalScale = 1.0f;
    int occlusionTexture = -1;
    glm::vec3 emissiveFactor{0.0f}; int emissiveTexture = -1;
    bool doubleSided = false;
    int alphaMode = 0;                      // 0=OPAQUE 1=MASK 2=BLEND
    float alphaCutoff = 0.5f;
};

struct GLTFRawFile {
    std::vector<GLTFNodeInfo> nodes;
    std::vector<int> sceneRoots;            // 默认场景的根节点索引
    std::vector<GLTFRawMesh> meshes;
    std::vector<GLTFRawImage> images;
    std::vector<GLTFRawMaterial> materials;
};
```

**API**：

```cpp
namespace GE {
// 解析 .gltf（LoadASCIIFromFile）/ .glb（LoadBinaryFromFile）→ 引擎中间结构。
bool ParseGLTF(const std::string &filepath, GLTFRawFile &out, std::string &err);

// 构建单个 glTF mesh 为 Mesh（key 形如 "path.glb#mesh3"）。同步：顶点装配 + 切线 + GPU 上传。
std::unique_ptr<Mesh> BuildGLTFMesh(VulkanDevice &device, const GLTFRawFile &raw,
                                    int meshIndex, const std::string &key);

// 构建单个 glTF 材质（注册进 MaterialManager，key = "path#mat{材质索引}"，幂等复用）。
Material *BuildGLTFMaterial(MaterialManager &matMgr, TextureManager &texMgr,
                            const GLTFRawFile &raw, int matIndex, const std::string &filepath);

// 把某 mesh 各子网格的默认材质挂上（按 primitiveMaterials 逐一下移）。
void AssignGLTFSubMeshMaterials(Mesh &mesh, MaterialManager &matMgr, TextureManager &texMgr,
                                const GLTFRawFile &raw, int meshIndex, const std::string &filepath);
}
```

**顶点装配规则**（`BuildGLTFMesh` 内）：
- 位置/法线/UV 从 accessor 读入；**UV 不翻转**（glTF V 轴天然匹配 Vulkan，与 OBJ 的 `1-v` 相反）
- 全部字段经 `Vertex::Quantize` 量化后按完整 `Vertex` 去重（与 OBJ 路径一致的精确比较）
- 无 `NORMAL` accessor → 按面计算平坦法线；无 `TEXCOORD_0` → 本 mesh 跳过切线（`ComputeTangents` 除零防御）
- `TANGENT` accessor 缺失时交由 `Mesh::Create` 的共享 `ComputeTangents` 补齐
- 多 UV 集只取 `TEXCOORD_0`（高集支持后续）；忽略 skin/morph/animation 数据
- `node.matrix` 为列主序 16 元数组 → `glm::make_mat4`；TRS 形式则直接映射（rotation: x,y,z,w → glm::quat(w,x,y,z)）

**材质规则**（`BuildGLTFMaterial` 内）：
- 类型恒为 `Material::Type::PBR`；`metallicFactor`/`roughnessFactor` → `SetFloat("metallic"/"roughness")`
  （着色器已按 `mr.b * pbr.x / mr.g * pbr.y` 乘贴图，无贴图时默认 `(G=1,B=1)` 纹理兜底 = 纯标量因子）
- `emissiveFactor` → `SetEmissiveFactor`（着色器已消费，两类型共用）
- 贴图槽位（格式：baseColor 走 sRGB，其余走 Unorm，数据纹理非颜色不加解码）：
  - **外部文件图片** → `TextureManager::LoadAsync(绝对路径, 格式)`（复用现成异步纹理解码管线）
  - **内嵌（GLB bufferView / data URI）** → `TextureManager::LoadFromMemoryAsync(key="path#image{图片索引}", 像素, w, h, 格式)`
  - **baseColor 无贴图** → 纯色纹理，但**格式用 linear（`eR8G8B8A8Unorm`）**：glTF baseColorFactor 是线性值，
    塞进 sRGB 纯色贴图会被二次解码偏亮（本方案要点之一）
  - **baseColor 有贴图** → sRGB 贴图；若 baseColorFactor 明显非白，本阶段记录并告警（factor×贴图需要
    着色器多一个因子通道，暂缓）
  - metallicRoughness 贴图 → `Material::MetallicRoughness` 槽位（渲染端已消费，零渲染改动）；
    normal 贴图 → `Material::Normal`；emissive 贴图 → `Material::Emissive`；occlusion 解析但**不绑定**（着色器未采样）
- 渲染状态**仅记录**：`doubleSided → Material::doubleSided`、`alphaMode=MASK → Material::alphaTest` +
  `SetFloat("alphaCutoff", cutoff)`、`normalScale → SetFloat("normalScale")`（渲染端生效后续）

### 4.2 `GE/include/GE/Render/GLTFImporter.h` + `GE/src/Render/GLTFImporter.cpp`

```cpp
namespace GE {
class GLTFImporter {
public:
    // 两阶段异步导入：立即返回；后台解析+上传，主线程 Poll 回收时建实体树并回调。
    // onDone(bool ok, std::vector<Entity> roots)：ok=false 为解析失败；roots 为空实体列表。
    static void ImportAsync(Scene &scene, const std::string &filepath,
                            std::function<void(bool, std::vector<Entity>)> onDone = {});

    // 作废所有在途导入（编辑器切换/新建场景时调用，防止 finalize 写入已销毁 Scene 的 use-after-free）。
    static void AbortAllPending();
};
}
```

**ImportAsync 内部**（单个 AsyncUploadManager::UploadTask，共享 `shared_ptr<GLTFImportSession>`）：
- `decode`（后台）：`ParseGLTF` → `GLTFRawFile`；逐 mesh 调用 `ComputeTangents`（若该 mesh 有 UV）
- `upload`（后台）：逐 glTF mesh 用 staging + 录拷贝在命令缓冲上建 GPU 本地顶点/索引缓冲
  （复用 `LoadFromFileAsync` 的 recordCopy 写法），staging 交 `task.staging` 主线程回收
- `finalize`（主线程 Poll）：先查 `session->aborted`（场景已被换掉则直接返回）
  1. 逐 mesh 用 `Mesh::CreateFromBuffers` 组装 `Mesh`（CPU 数组 + 已上传缓冲，仅组装不上传）→
     注册进 `MeshManager`，key=`path#mesh{网格索引}`（已存在则跳过）
  2. 逐材质 `BuildGLTFMaterial` → 逐子网格 `AssignGLTFSubMeshMaterials` 挂默认材质
  3. 递归建实体树：创建根实体（文件 stem 命名）→ 对 `sceneRoots` 递归
     - 每 node：`CreateEntity(node.name)` → 局部 TRS（matrix 用 `glm::decompose` 拆成 T/R/S）
     - 有 mesh：`AddComponent<MeshRendererComponent>(构造好的 Mesh*)`
     - 递归子节点 → `Scene::SetParent(子, 父)`（阶段 1 单一入口，含环检测）
  4. `onDone(true, {根实体})`；解析失败时 `onDone(false, {})`

**AbortAllPending 防护**：`ImportAsync` 捕获 `Scene*` + 会话对象（含 `std::atomic<bool> aborted`）。
编辑器在切换/新建/加载场景时调 `AbortAllPending()` 置位所有在途会话；其 finalize 见到 `aborted`
即返回，不触碰已销毁的 Scene。Mesh/Material 仍照常注册（资源层无害，可按 key 复用）。

---

## 5. 改动现有文件

### 5.1 `Mesh.h` / `Mesh.cpp`
- 加重载（同步构建带子网格/材质数据/文件路径的网格，走共享装配 `BuildMesh`）：
  ```cpp
  static std::unique_ptr<Mesh> Create(VulkanDevice &device,
                                      std::vector<Vertex> vertices,
                                      std::vector<uint32_t> indices,
                                      std::vector<SubMesh> subMeshes,
                                      std::vector<MaterialData> materialData,
                                      std::string filePath);
  ```
- 加工厂（接住后台已上传的 GPU 缓冲，**只组装不重传**；内部复用 `InstallAsyncData` 的成员安装）：
  ```cpp
  static std::unique_ptr<Mesh> CreateFromBuffers(
      std::vector<Vertex> vertices,
      std::vector<uint32_t> indices,
      std::vector<SubMesh> subMeshes,
      std::vector<MaterialData> materialData,
      std::string filePath,
      std::unique_ptr<VulkanBuffer> vertexBuffer,
      std::unique_ptr<VulkanBuffer> indexBuffer);
  ```
  > 注：若不保留该工厂，异步 finalize 只能同步重建 GPU 缓冲（同一份数据传两遍、导入完成瞬间卡主线程），
  > 与「两阶段异步不卡主线程」的目标相悖，故保留。
- `ComputeTangents`：对 UV 微分叉积接近 0 的三角形跳过累积（glTF 允许无 UV，防除零产生 NaN 切线）。

### 5.2 `TextureManager.h` / `.cpp`
- 加内存加载入口（GLB 内嵌贴图必经之路），内部复用 `Texture::LoadFromMemoryAsync` / `LoadFromMemory` + `Register` 缓存：
  ```cpp
  Texture *LoadFromMemoryAsync(const std::string &key, const void *pixels,
                               uint32_t width, uint32_t height,
                               vk::Format format = vk::Format::eR8G8B8A8Unorm);
  Texture *LoadFromMemory(const std::string &key, const void *pixels,
                          uint32_t width, uint32_t height,
                          vk::Format format = vk::Format::eR8G8B8A8Unorm);
  ```

### 5.3 `MeshManager.cpp`（无新公开 API）
- 现有 `Load()` 增加对 `path#mesh{n}` 键的识别（解析尾部 `#mesh` + 数字）：
  1. 缓存命中直接返回（含 Import 已注册的场景）
  2. 否则主线程同步 `ParseGLTF` + `BuildGLTFMesh` + `BuildGLTFMaterial`/`AssignGLTFSubMeshMaterials` + 注册
  - 作用：场景序列化回读 `LoadMesh("path.glb#mesh0")` 时兜底（重开进程/未走 Import 也能还原），
    与 Import 结果同 key 幂等；裸 `.gltf/.glb` 路径（无 `#mesh`）不在 Load 语义内，编辑器走 `ImportAsync`。

### 5.4 编辑器（GE_Editor）
- `SceneHierarchyPanel`「加载模型文件 (OBJ)…」→ 改为「加载模型文件…」，文件过滤器加 `.gltf;*.glb`：
  - `.obj` → 维持 `meshMgr.Load`
  - `.gltf/.glb` → `GLTFImporter::ImportAsync(scene, path, cb)`；按钮旁显示「导入中…」，完成后
    若当前有选中实体则把根实体挂到其下（`Scene::SetParent`），否则作为场景根
- `SceneLayer` 切换/新建/加载场景处调用 `GLTFImporter::AbortAllPending()`

### 5.5 根 `CMakeLists.txt`
- `GE_SRC` glob 加 `${CMAKE_SOURCE_DIR}/GE/third_party/tinygltf/tiny_gltf.cc`
- `target_include_directories` 加 `${CMAKE_SOURCE_DIR}/GE/third_party/tinygltf`
- `set_source_files_properties(tiny_gltf.cc PROPERTIES COMPILE_DEFINITIONS "STB_IMAGE_STATIC")`
  → 使 tinygltf 内的 stbi 实现成为本 TU 内部静态符号，**避免与引擎 `GE/third_party/stb/stb_image.cpp`
  （外部符号 stbi_load 等）重复定义导致链接失败**；引擎侧 stbi 仍由 `stb_image.cpp` 提供、Texture.cpp 使用

---

## 6. 关键流程

### 6.1 编辑器导入时序

```
主线程 用户点按钮 → GLTFImporter::ImportAsync(scene, "assets/models/buster_drone.glb", cb)
  │ 组装 UploadTask（捕获 scene* + session）
  ▼
后台上传线程 decode：tinygltf 解析 → GLTFRawFile（顶点/UV/贴图像素/节点树）
  ▼
后台上传线程 upload：逐 mesh 建顶点/索引缓冲 + 录拷贝 → 提交 GPU
  ▼（每帧主线程 Renderer::BeginFrame → AsyncUploadManager::Poll()，fence 完成）
主线程     finalize：组装 Mesh → 注册 MeshManager → 建材质（贴图异步加载，未就绪渲染自动降级）
                        → 递归建实体树 → SetParent → cb(true, roots)
```

### 6.2 网格复用与序列化

```
多 node 引用同一 glTF mesh   → 同一 key "path#mesh{索引}" → 同一 Mesh*（显存只一份）
场景保存                    → 每实体记 Tag / Transform(局部 TRS) / Mesh key / Parent(UUID)，全走阶段 1 序列化
场景重开                    → LoadMesh("path#mesh{索引}")：缓存命中 或 同步 ParseGLTF+创建 → 层级/变换/网格还原
```

---

## 7. 验收标准

- 导入 `assets/models/buster_drone.glb`（GLB 内嵌贴图）与 `assets/models/adamHead/adamHead.gltf`（外置 .bin）
  生成实体树：层级面板可见树形结构，各带网格 node 为独立实体、局部坐标正确
- 材质：PBR 金属-粗糙度（MR 贴图/因子 × 贴图）、法线贴图、自发光正确显示；GLB 内嵌贴图正常加载；
  纯色 baseColorFactor 颜色不偏亮
- 保存场景 → 重载：实体树层级、局部变换、网格引用完整还原
- 子节点可独立旋转（阶段 1 父子 + 本阶段导入共同达成）
- 引用同一网格的多个 node 共享同一 Mesh*（资源面板网格列表去重可见）
- 大文件导入期间编辑器不冻结（解析/上传在后台，主线程仅 Poll 组装）

---

## 8. 与原方案（`docs/glTF导入与骨骼动画实施方案.md`）的偏离记录

| 原方案 | 本方案 | 原因 |
|---|---|---|
| 2.1 扩展 `ParseModelData` 注册 `.gltf/.glb` | 独立 `GLTFRawLoader` + `GLTFImporter` | `ParseModelData` 输出「单个 Mesh 的几何+材质」，装不下「多 mesh + node 层级 + 局部变换 + 实体树」；且实体树必须在主线程建，异步 decode 线程无法产出实体 |
| 2.3 「一文件一 Mesh」扩展为 `ImportedAsset` 返回多实体 | 网格走 `MeshManager` 的 `path#mesh{n}` 键缓存 + 实体树由 `GLTFImporter` 直接建 | 序列化回读需要稳定 per-mesh key，`LoadMesh("path#mesh{n}")` 兜底即等价于 `ImportedAsset` 的资源侧 |
| 同步/异步两条加载路径共用 `ParseModelData` | 编辑器导入**两阶段异步**（后台解析+上传、主线程建树）；场景回读**同步** | 实体树主线程约束；回读在场景加载流程本就同步 |
| 2.2 `doubleSided`/`alphaMode` 承接 | 字段记录但不渲染生效 | 已拍板：渲染端（cull / discard / normalScale）后续单独做 |
| —— | `mesh_pbr.frag` 已采样 MR 贴图（binding 4），渲染端零改动 | 阶段 1 材质系统已铺好（metallic/roughness 因子 × 贴图） |
| —— | 内嵌贴图走 `TextureManager::LoadFromMemoryAsync` | GLB bufferView / data URI 无文件路径，无法走文件加载 |

**暂缓清单（后续阶段）**：doubleSided 双面渲染、alphaMask discard、normalScale、occlusion 采样、
baseColorFactor × 有贴图的因子、骨骼/蒙皮（阶段 3）、动画（阶段 4）。