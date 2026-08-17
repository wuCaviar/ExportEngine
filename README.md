# ExportEngine

矢量图形 → CMYK TIFF 渲染引擎。读取 JSON 场景描述文件，渲染为带 ICC 色彩管理的 CMYK TIFF 印刷文件。

## 功能特性

- **6 种图元**：矩形（支持圆角）、椭圆/圆、自由折线、贝塞尔曲线、直线、文字
- **文字渲染**：实心填充抗锯齿字形，支持字体家族、字号、粗体、斜体、对齐
- **CMYK 色彩管理**：图元颜色 CMYK-only（LittleCMS ICC 转换）
- **渐变填充**：线性渐变、径向渐变、锥形渐变
- **线条样式**：实线、虚线、点线、点划线、双点划线
- **抗锯齿渲染**：SDF 填充 + AGG 库描边，高质量抗锯齿输出
- **多平台**：macOS（FreeType + CoreText）、Windows（GDI Win32）
- **TIFF 输出**：CMYK 8 位、LZW 压缩、内嵌 ICC Profile

## 构建

### macOS

```bash
# 安装依赖
brew install freetype libtiff little-cms2 cmake

# 配置并构建
cmake -B build -S .
make -C build -j$(sysctl -n hw.ncpu)

# 构建产物: bin/Debug/
#   libExportEngine.dylib  — 核心渲染库
#   EETool                 — 命令行工具
#   EEVerify               — 验证工具
```

### Windows

使用 Visual Studio 2022 打开 `ExportEngine.sln`，选择 `x64 Debug` 或 `x64 Release` 编译。

构建产物位于 `bin/<Configuration>/` 目录：

- `ExportEngine.dll` — 核心渲染库
- `EETool.exe` — 命令行工具
- `ICC Profile/` — ICC 色彩配置文件

### 第三方依赖

| 库                                                | 用途                    | 平台          |
| ------------------------------------------------- | ----------------------- | ------------- |
| [AGG](https://github.com/cppfw/agg)               | 抗锯齿光栅化 + 字体引擎 | 全平台        |
| [FreeType](https://freetype.org/)                 | 字体轮廓提取            | macOS / Linux |
| [LittleCMS 2](https://littlecms.com/)             | ICC 色彩管理            | 全平台        |
| [libtiff](https://libtiff.gitlab.io/libtiff/)     | TIFF 文件写入           | 全平台        |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 解析               | 全平台        |

macOS 额外使用 CoreText / CoreFoundation 系统框架进行字体名称查找。

## 使用方法

### 命令行

```bash
# 单个 JSON → TIFF 转换
EETool <input.json> -o <output.tiff>

# 批量转换文件夹内所有 .json 文件
EETool <input_dir/> -o <output_dir/>
```

示例：

```bash
# 单个文件
EETool.exe test_scene.json -o output.tif

# 批量转换
EETool.exe Test/rects/ -o output/rects/
```

| 参数            | 说明                                                         |
| --------------- | ------------------------------------------------------------ |
| `<input>`       | 输入 JSON 文件或文件夹（必需）                               |
| `-o <output>`   | 输出路径（必需）：单文件→指定 .tif 文件，文件夹→指定输出目录 |
| `-h, --help`    | 显示帮助信息                                                 |
| `-v, --version` | 显示版本号                                                   |

### C++ API

```cpp
#include "Json2Tiff.h"

// JSON → TIFF（默认颜色转换 + 最近邻缩放 + 写 TIFF，ICC 内部自管）
bool ok = json2tiff("scene.json", "output.tif",
    [](int cur, int total, const std::string &msg) {
        printf("[%d/%d] %s\n", cur, total, msg.c_str());
    },
    [](std::error_code ec) {
        fprintf(stderr, "Error: %s\n", ec.message().c_str());
    });
```

自定义颜色转换 / 像素缩放 / 输出接收器见上文「可插拔服务（模块化导出）」。颜色转换、缩放、输出后处理、ICC 均不经过画布 JSON——引擎与 sink 各自内部管理。

### 纯代码构建场景（不使用 JSON）

```cpp
Canvas canvas;
canvas.width = 2480;
canvas.height = 3508;
canvas.dpi.x = 300;
canvas.dpi.y = 300;
canvas.background = Color::fromCMYK(0, 0, 0, 0);

Rect r;
r.x = 100; r.y = 100;
r.width = 500; r.height = 300;
r.fillColor = Color::fromCMYK(0, 100, 100, 0);   // 红色
r.strokeColor = Color::fromCMYK(0, 0, 0, 100);    // 黑色
r.strokeWidth = 2;
r.z = 1;
canvas.rects.push_back(r);
```

### 可插拔服务（模块化导出）

ExportEngine 的渲染管线是「数据整合 + 可插拔策略」：EE 只负责 JSON→Canvas 建模、图元
调度、预算预解码、像素合成，颜色转换 / 像素缩放 / 渲染输出接收器均由接口注入，
默认实现为 LittleCMS2 + vips 最近邻 + 写 TIFF。

| 接口 | 职责 | 默认实现 |
| --- | --- | --- |
| `IColorConverter` | 源 → CMYK 颜色转换 | `ColorConverter`（LittleCMS2） |
| `IResampler` | 像素缩放 | `NearestResampler`（vips 最近邻） |
| `IRenderSink` | 渲染输出接收器（最终去向） | `TiffSink`；另有 `MemorySink`/`CallbackSink`/`NullSink` |

```cpp
#include "Json2Tiff.h"

RenderServices services;                          // 全空 = 引擎默认

// 回传内存（不经文件）
auto mem = std::make_shared<MemorySink>();
services.sink = mem;
if (json2sink("scene.json", services)) {
    auto &frame = mem->frame();       // width×height×4 的 CMYK 像素
    auto &desc  = mem->descriptor();  // 尺寸/DPI/通道/ICC
}

// 或写 TIFF（便捷入口，默认服务 + TiffSink）
json2tiff("scene.json", "out.tif");
```

自定义策略：继承对应接口并实现即可，例如：

```cpp
// 自定义输出接收器：丢弃所有像素（仅压测）
struct NullSink : IRenderSink {
    SinkGranularity granularity() const noexcept override { return SinkGranularity::Strip; }
    bool begin(const SinkDescriptor &) override { return true; }
    bool writeStrip(int, int, std::vector<uint8_t>) override { return true; }
    bool end() override { return true; }
};
RenderServices svc;
svc.sink = std::make_shared<NullSink>();
json2sink("scene.json", svc);
```

## JSON 场景描述文件

### 整体结构

```json
{
    "canvas": {
        "width": 800,
        "height": 600,
        "dpi": 300,
        "background": { ... },
        "rects": [ ... ],
        "circles": [ ... ],
        "freeLines": [ ... ],
        "bezierCurves": [ ... ],
        "lines": [ ... ],
        "texts": [ ... ]
    }
}
```

| 字段          | 类型     | 说明                                                           |
| ------------- | -------- | -------------------------------------------------------------- |
| `width`       | 整数     | 画布宽度（单位取决于 `unit` 字段）                             |
| `height`      | 整数     | 画布高度（单位取决于 `unit` 字段）                             |
| `unit`        | 字符串   | 画布单位，可选 `"px"`（默认）或 `"mm"`。图元未标单位时继承此值 |
| `dpi`         | 整数     | 输出分辨率，默认 300                                           |
| `background`  | 颜色对象 | 背景颜色                                                       |

### 颜色对象

图元（非图片）颜色仅支持 CMYK（0-100%）。`space` 字段与 RGB（r/g/b）颜色已移除——出现即报错。

**CMYK 颜色**（百分比，0-100）：

```json
{
  "c": 0,
  "m": 100,
  "y": 100,
  "k": 0,
  "a": 1.0
}
```

| 字段 | 范围    | 说明                       |
| ---- | ------- | -------------------------- |
| `c`  | 0-100   | 青色                       |
| `m`  | 0-100   | 品红                       |
| `y`  | 0-100   | 黄色                       |
| `k`  | 0-100   | 黑色                       |
| `a`  | 0.0-1.0 | 不透明度（可选，默认 1.0） |

**颜色迁移**（旧格式）：

- 删除 `"space"` 键：`{ "space": "cmyk", "c": 0, ... }` → `{ "c": 0, ... }`
- RGB 颜色需转换为 CMYK：红 `{ "r": 255, "g": 0, "b": 0 }` → `{ "c": 0, "m": 100, "y": 100, "k": 0 }`；白 → `{ "c": 0, "m": 0, "y": 0, "k": 0 }`
- 旧格式不会被静默接受：`space` 或 `r`/`g`/`b` 键 → 解析错误 / 校验 Error

### 渐变填充

渐变是图元（Rect / Circle）上的独立字段，与 `fillColor` 互斥。使用 `gradient` 替代 `fillColor` 即可：

**线性渐变**：

```json
{
  "x": 50,
  "y": 50,
  "width": 400,
  "height": 300,
  "gradient": {
    "type": "linear",
    "x1": 0,
    "y1": 0,
    "x2": 1,
    "y2": 0,
    "stops": [
      {
        "offset": 0.0,
        "color": {
          "c": 0,
          "m": 100,
          "y": 100,
          "k": 0,
          "a": 1.0
        }
      },
      {
        "offset": 1.0,
        "color": { "c": 100, "m": 0, "y": 0, "k": 0, "a": 1.0 }
      }
    ]
  }
}
```

**径向渐变**：

```json
{
  "x": 50,
  "y": 50,
  "width": 400,
  "height": 300,
  "gradient": {
    "type": "radial",
    "cx": 0.5,
    "cy": 0.5,
    "r": 0.5,
    "stops": [
      {
        "offset": 0.0,
        "color": { "c": 0, "m": 0, "y": 0, "k": 0, "a": 1.0 }
      },
      {
        "offset": 1.0,
        "color": {
          "c": 0,
          "m": 100,
          "y": 100,
          "k": 50,
          "a": 1.0
        }
      }
    ]
  }
}
```

**锥形渐变**：

```json
{
  "x": 50,
  "y": 50,
  "width": 400,
  "height": 300,
  "gradient": {
    "type": "conic",
    "cx": 0.5,
    "cy": 0.5,
    "startAngle": 0,
    "stops": [
      {
        "offset": 0.0,
        "color": { "c": 100, "m": 0, "y": 0, "k": 0, "a": 1.0 }
      },
      {
        "offset": 0.33,
        "color": { "c": 0, "m": 100, "y": 0, "k": 0, "a": 1.0 }
      },
      {
        "offset": 0.66,
        "color": { "c": 0, "m": 0, "y": 100, "k": 0, "a": 1.0 }
      },
      {
        "offset": 1.0,
        "color": { "c": 100, "m": 0, "y": 0, "k": 0, "a": 1.0 }
      }
    ]
  }
}
```

渐变参数说明：

| 参数         | 适用类型      | 说明                              |
| ------------ | ------------- | --------------------------------- |
| `x1`, `y1`   | linear        | 起点坐标（归一化 0-1）            |
| `x2`, `y2`   | linear        | 终点坐标（归一化 0-1）            |
| `cx`, `cy`   | radial, conic | 中心点坐标（归一化 0-1）          |
| `r`          | radial        | 半径（归一化 0-1）                |
| `startAngle` | conic         | 起始角度（度）                    |
| `offset`     | 所有          | 色标位置（0.0-1.0），需按升序排列 |

### 图元类型

#### 矩形（rects）

```json
{
  "x": 50,
  "y": 50,
  "width": 400,
  "height": 300,
  "z": 1,
  "fillColor": {
    "c": 0,
    "m": 100,
    "y": 100,
    "k": 0,
    "a": 1.0
  },
  "strokeColor": {
    "c": 0,
    "m": 0,
    "y": 0,
    "k": 100,
    "a": 1.0
  },
  "strokeWidth": 2,
  "cornerRadius": 20,
  "lineStyle": "solid"
}
```

| 字段           | 类型     | 默认值   | 说明                                            |
| -------------- | -------- | -------- | ----------------------------------------------- |
| `x`            | 数字     | 0        | 左上角 X 坐标（受 `unit` 影响）                 |
| `y`            | 数字     | 0        | 左上角 Y 坐标（受 `unit` 影响）                 |
| `width`        | 数字     | 0        | 宽度（受 `unit` 影响）                          |
| `height`       | 数字     | 0        | 高度（受 `unit` 影响）                          |
| `z`            | 数字     | 0        | 图层顺序（数值小的在底层）                      |
| `unit`         | 字符串   | 继承画布 | 单位，可选 `"px"` 或 `"mm"`                     |
| `fillColor`    | 颜色对象 | —        | 填充颜色，与 `gradient`/`gridFill`/`textureFill` 互斥 |
| `gradient`     | 渐变对象 | —        | 渐变填充，与 `fillColor`/`gridFill`/`textureFill` 互斥（仅 Rect/Circle） |
| `gridFill`     | 对象     | —        | 网格填充，与 `fillColor`/`gradient`/`textureFill` 互斥（优先级次高） |
| `textureFill`  | 对象     | —        | 图片纹理填充，与 `fillColor`/`gradient`/`gridFill` 互斥（优先级最高） |
| `strokeColor`  | 颜色对象 | —        | 描边颜色                                        |
| `strokeWidth`  | 数字     | 0        | 描边宽度（受 `unit` 影响），0 表示不描边        |
| `cornerRadius` | 数字     | 0        | 圆角半径（受 `unit` 影响），0 表示直角          |
| `lineStyle`    | 字符串   | "solid"  | 描边样式，见下方线型说明                        |

网格填充（`gridFill`）示例：

```json
{
  "x": 1150, "y": 50, "width": 500, "height": 350, "z": 3,
  "gridFill": {
    "gridColor": { "c": 0, "m": 0, "y": 0, "k": 100, "a": 1 },
    "backgroundColor": { "c": 0, "m": 13.7, "y": 29.4, "k": 0, "a": 1 }
  }
}
```

说明：

- 与 `fillColor`/`gradient`/`textureFill` 互斥；同时给出多个填充时仅取一个，优先级 `textureFill > gridFill > gradient > fillColor`（其余忽略并输出告警）。
- `transparentBackground: true` 时不绘制格子背景色（格间透明）。
- 网格尺寸固定，不可配置：格子 = 1mm、线宽 = 1px，由画布 `dpi` 换算（`1mm = dpi / 25.4` 像素，如 300dpi 下每格 ≈ 11.8px）。旧的 `cellWidth`/`cellHeight`/`lineWidth` 字段已废弃，不再生效（输出告警）。

图片纹理填充（`textureFill`）示例：

```json
{
  "x": 600, "y": 50, "width": 500, "height": 350, "z": 2,
  "textureFill": {
    "filePath": "Test/images/win/pattern_alpha.png",
    "useOriginalSize": true
  }
}
```

说明：

- 与 `fillColor`/`gradient`/`gridFill` 互斥（优先级最高）；同一矩形同时给出 `fillColor` + `textureFill` 时渲染取 `textureFill`。
- `useOriginalSize: true`：按原图像素尺寸 × 画布 DPI / 图片 DPI 换算（PNG 无内嵌 DPI 时按 72 处理），如 300dpi 画布下 8px 原图每格 ≈ 33px（300/72 ≈ 4.17 倍）。
- `useOriginalSize: false`：按 `customWidth`/`customHeight` 平铺（受 `unit` 影响）；`offsetX`/`offsetY` 为平铺偏移（受 `unit` 影响）。
- `filePath` 为相对路径时相对当前运行目录（从仓库根目录运行 EETool）。

#### 椭圆/圆（circles）

```json
{
  "cx": 300,
  "cy": 300,
  "radiusX": 100,
  "radiusY": 60,
  "z": 2,
  "fillColor": { "c": 0, "m": 60, "y": 100, "k": 0, "a": 1.0 },
  "strokeColor": {
    "c": 0,
    "m": 0,
    "y": 0,
    "k": 100,
    "a": 1.0
  },
  "strokeWidth": 2,
  "lineStyle": "dashed"
}
```

| 字段          | 类型     | 默认值   | 说明                                            |
| ------------- | -------- | -------- | ----------------------------------------------- |
| `cx`          | 数字     | 0        | 中心 X 坐标（受 `unit` 影响）                   |
| `cy`          | 数字     | 0        | 中心 Y 坐标（受 `unit` 影响）                   |
| `radiusX`     | 数字     | 0        | X 轴半径（受 `unit` 影响）                      |
| `radiusY`     | 数字     | radiusX  | Y 轴半径（受 `unit` 影响），省略则为正圆        |
| `z`           | 数字     | 0        | 图层顺序                                        |
| `unit`        | 字符串   | 继承画布 | 单位，可选 `"px"` 或 `"mm"`                     |
| `fillColor`   | 颜色对象 | —        | 填充颜色，与 `gradient` 互斥                    |
| `gradient`    | 渐变对象 | —        | 渐变填充，与 `fillColor` 互斥（仅 Rect/Circle） |
| `strokeColor` | 颜色对象 | —        | 描边颜色                                        |
| `strokeWidth` | 数字     | 0        | 描边宽度（受 `unit` 影响）                      |
| `lineStyle`   | 字符串   | "solid"  | 描边样式                                        |

#### 自由折线（freeLines）

```json
{
  "points": [
    { "x": 100, "y": 200 },
    { "x": 200, "y": 150 },
    { "x": 300, "y": 250 }
  ],
  "z": 3,
  "strokeColor": {
    "c": 0,
    "m": 100,
    "y": 0,
    "k": 0,
    "a": 1.0
  },
  "strokeWidth": 2,
  "lineStyle": "dotted"
}
```

| 字段          | 类型     | 默认值   | 说明                                            |
| ------------- | -------- | -------- | ----------------------------------------------- |
| `points`      | 数组     | —        | 顶点坐标数组，至少 2 个点（坐标受 `unit` 影响） |
| `z`           | 数字     | 0        | 图层顺序                                        |
| `unit`        | 字符串   | 继承画布 | 单位，可选 `"px"` 或 `"mm"`                     |
| `strokeColor` | 颜色对象 | —        | 线条颜色                                        |
| `strokeWidth` | 数字     | 1        | 线条宽度（受 `unit` 影响）                      |
| `lineStyle`   | 字符串   | "solid"  | 线条样式                                        |

#### 贝塞尔曲线（bezierCurves）

```json
{
  "controlPoints": [
    { "x": 50, "y": 300 },
    { "x": 150, "y": 100 },
    { "x": 350, "y": 500 },
    { "x": 450, "y": 200 }
  ],
  "z": 4,
  "strokeColor": {
    "c": 50,
    "m": 0,
    "y": 100,
    "k": 0,
    "a": 1.0
  },
  "strokeWidth": 2,
  "lineStyle": "solid"
}
```

| 字段            | 类型     | 默认值   | 说明                                |
| --------------- | -------- | -------- | ----------------------------------- |
| `controlPoints` | 数组     | —        | 控制点数组（坐标受 `unit` 影响）    |
|                 |          |          | 2 个点 → 直线                       |
|                 |          |          | 3 个点 → 二次贝塞尔                 |
|                 |          |          | 4 个点 → 三次贝塞尔                 |
|                 |          |          | 7 个点 → 两段三次贝塞尔（首尾共享） |
| `z`             | 数字     | 0        | 图层顺序                            |
| `unit`          | 字符串   | 继承画布 | 单位，可选 `"px"` 或 `"mm"`         |
| `strokeColor`   | 颜色对象 | —        | 线条颜色                            |
| `strokeWidth`   | 数字     | 1        | 线条宽度（受 `unit` 影响）          |
| `lineStyle`     | 字符串   | "solid"  | 线条样式                            |

#### 直线（lines）

```json
{
  "x1": 100,
  "y1": 200,
  "x2": 500,
  "y2": 400,
  "z": 5,
  "strokeColor": {
    "c": 0,
    "m": 0,
    "y": 0,
    "k": 100,
    "a": 1.0
  },
  "strokeWidth": 1,
  "lineStyle": "dashDot"
}
```

| 字段          | 类型     | 默认值   | 说明                        |
| ------------- | -------- | -------- | --------------------------- |
| `x1`          | 数字     | 0        | 起点 X（受 `unit` 影响）    |
| `y1`          | 数字     | 0        | 起点 Y（受 `unit` 影响）    |
| `x2`          | 数字     | 0        | 终点 X（受 `unit` 影响）    |
| `y2`          | 数字     | 0        | 终点 Y（受 `unit` 影响）    |
| `z`           | 数字     | 0        | 图层顺序                    |
| `unit`        | 字符串   | 继承画布 | 单位，可选 `"px"` 或 `"mm"` |
| `strokeColor` | 颜色对象 | —        | 线条颜色                    |
| `strokeWidth` | 数字     | 1        | 线条宽度（受 `unit` 影响）  |
| `lineStyle`   | 字符串   | "solid"  | 线条样式                    |

#### 文字（texts）

```json
{
  "x": 50,
  "y": 50,
  "z": 6,
  "content": "Hello World",
  "fontFamily": "Arial",
  "fontSize": 36,
  "bold": false,
  "italic": false,
  "textColor": {
    "c": 100,
    "m": 0,
    "y": 0,
    "k": 0,
    "a": 1.0
  }
}
```

| 字段         | 类型     | 默认值   | 说明                                      |
| ------------ | -------- | -------- | ----------------------------------------- |
| `x`, `y`     | 数字     | 0        | 文字左上角坐标（受 `unit` 影响）          |
| `z`          | 数字     | 0        | 图层顺序                                  |
| `unit`       | 字符串   | 继承画布 | 单位，可选 `"px"` 或 `"mm"`               |
| `content`    | 字符串   | —        | 文字内容（UTF-8）                         |
| `fontFamily` | 字符串   | —        | 字体家族名称（如 "Arial"、"PingFang SC"） |
| `fontSize`   | 数字     | 16       | 字号（pt，不受 `unit` 影响）              |
| `bold`       | 布尔     | false    | 粗体                                      |
| `italic`     | 布尔     | false    | 斜体                                      |
| `textColor`  | 颜色对象 | —        | 文字颜色                                  |

**平台字体说明**：macOS 通过 CoreText 将 `fontFamily` 解析为系统字体路径。若指定的字体未安装，文字将不渲染。中文推荐使用 macOS 系统字体如 `"PingFang SC"`、`"STSong"`、`"STHeiti"`。

#### 图片（images）

```json
{
  "filePath": "/path/to/image.tiff",
  "x": 100,
  "y": 100,
  "width": 400,
  "height": 300,
  "z": 5
}
```

| 字段       | 类型   | 默认值   | 说明                                                        |
| ---------- | ------ | -------- | ----------------------------------------------------------- |
| `filePath` | 字符串 | —        | TIFF 文件的绝对路径（仅支持 CMYK TIFF）                     |
| `x`        | 数字   | 0        | 放置位置 X（受 `unit` 影响）                                |
| `y`        | 数字   | 0        | 放置位置 Y（受 `unit` 影响）                                |
| `width`    | 数字   | 0        | 目标宽度（受 `unit` 影响），0 表示按 DPI 和原始尺寸自动计算 |
| `height`   | 数字   | 0        | 目标高度（受 `unit` 影响），0 表示按 DPI 和原始尺寸自动计算 |
| `unit`     | 字符串 | 继承画布 | 单位，可选 `"px"` 或 `"mm"`                                 |
| `z`        | 数字   | 0        | 图层顺序                                                    |

### 线条样式（lineStyle）

所有支持描边/线条的图元都可使用以下样式：

| 值                | 效果         |
| ----------------- | ------------ |
| `"solid"`         | 实线（默认） |
| `"dashed"`        | 虚线         |
| `"dotted"`        | 点线         |
| `"dashDot"`       | 点划线       |
| `"doubleDotDash"` | 双点划线     |
| `"none"`          | 不绘制描边   |

### 单位系统（unit）

ExportEngine 支持两种长度单位：`"px"`（像素）和 `"mm"`（毫米）。单位转换利用画布 `dpi` 字段完成：`mm → px = 值 × dpi / 25.4`。

**继承规则：**

- 画布 `unit` 字段定义全局默认单位，缺省为 `"px"`
- 每个图元可通过自身的 `unit` 字段覆盖单位
- 图元未指定 `unit` 时，继承画布的单位
- 不同图元可以使用不同单位，实现混合布局

**受 `unit` 影响的字段：** 位置坐标（`x`, `y`, `cx`, `cy`, `x1`, `y1`, `x2`, `y2`，点数组中的坐标）、几何尺寸（`width`, `height`, `radiusX`, `radiusY`）、描边宽度（`strokeWidth`）、圆角半径（`cornerRadius`）。

**不受 `unit` 影响的字段：** 图层顺序（`z`）、字号（`fontSize`，始终为 pt）、颜色值、线条样式等非长度属性。

**示例 — 完全使用毫米（A4 画布）：**

```json
{
  "canvas": {
    "unit": "mm",
    "width": 210,
    "height": 297,
    "dpi": 300,
    "rects": [
      {
        "x": 10,
        "y": 10,
        "width": 50,
        "height": 30,
        "z": 1,
        "fillColor": {
          "c": 0,
          "m": 100,
          "y": 100,
          "k": 0,
          "a": 1.0
        },
        "strokeWidth": 0.5
      }
    ]
  }
}
```

**示例 — 混合单位（像素画布，部分图元用毫米）：**

```json
{
  "canvas": {
    "unit": "px",
    "width": 2000,
    "height": 1500,
    "dpi": 240,
    "rects": [
      {
        "x": 50,
        "y": 50,
        "width": 400,
        "height": 300,
        "z": 1,
        "fillColor": {
          "c": 0,
          "m": 100,
          "y": 100,
          "k": 0,
          "a": 1.0
        }
      },
      {
        "unit": "mm",
        "x": 30,
        "y": 30,
        "width": 40,
        "height": 25,
        "z": 2,
        "fillColor": {
          "c": 100,
          "m": 0,
          "y": 0,
          "k": 0,
          "a": 0.8
        }
      }
    ]
  }
}
```

### 图层顺序（z-index）

所有图元共用同一个 z 轴排序空间。`z` 值越小越先绘制（在底层），越大越后绘制（在上层）。不同类型的图元（rects、circles、lines 等）之间也按 z 值排序。

### 完整示例

```json
{
  "canvas": {
    "width": 800,
    "height": 600,
    "dpi": 300,
    "background": {
      "c": 0,
      "m": 0,
      "y": 0,
      "k": 0,
      "a": 1.0
    },
    "rects": [
      {
        "x": 50,
        "y": 50,
        "width": 300,
        "height": 200,
        "z": 1,
        "fillColor": {
          "c": 0,
          "m": 100,
          "y": 100,
          "k": 0,
          "a": 1.0
        },
        "strokeColor": {
          "c": 0,
          "m": 0,
          "y": 0,
          "k": 100,
          "a": 1.0
        },
        "strokeWidth": 2,
        "cornerRadius": 10,
        "lineStyle": "solid"
      }
    ],
    "circles": [
      {
        "cx": 500,
        "cy": 300,
        "radiusX": 80,
        "radiusY": 80,
        "z": 2,
        "fillColor": {
          "c": 100,
          "m": 0,
          "y": 0,
          "k": 0,
          "a": 1.0
        },
        "strokeColor": {
          "c": 0,
          "m": 0,
          "y": 0,
          "k": 100,
          "a": 1.0
        },
        "strokeWidth": 1,
        "lineStyle": "solid"
      }
    ],
    "freeLines": [
      {
        "points": [
          { "x": 100, "y": 400 },
          { "x": 200, "y": 350 },
          { "x": 300, "y": 420 },
          { "x": 400, "y": 380 }
        ],
        "z": 3,
        "strokeColor": {
          "c": 0,
          "m": 0,
          "y": 100,
          "k": 0,
          "a": 1.0
        },
        "strokeWidth": 2,
        "lineStyle": "dashed"
      }
    ],
    "bezierCurves": [
      {
        "controlPoints": [
          { "x": 450, "y": 400 },
          { "x": 550, "y": 300 },
          { "x": 650, "y": 500 },
          { "x": 750, "y": 400 }
        ],
        "z": 4,
        "strokeColor": {
          "c": 50,
          "m": 0,
          "y": 100,
          "k": 0,
          "a": 1.0
        },
        "strokeWidth": 2,
        "lineStyle": "solid"
      }
    ],
    "lines": [
      {
        "x1": 50,
        "y1": 550,
        "x2": 750,
        "y2": 550,
        "z": 5,
        "strokeColor": {
          "c": 0,
          "m": 0,
          "y": 0,
          "k": 50,
          "a": 1.0
        },
        "strokeWidth": 1,
        "lineStyle": "dotted"
      }
    ],
    "texts": [
      {
        "x": 100,
        "y": 250,
        "z": 6,
        "content": "Hello ExportEngine",
        "fontFamily": "Arial",
        "fontSize": 28,
        "bold": false,
        "italic": false,
        "textColor": {
          "c": 0,
          "m": 0,
          "y": 0,
          "k": 100,
          "a": 1.0
        }
      }
    ],
    "images": [
      {
        "filePath": "/path/to/logo.tiff",
        "x": 600,
        "y": 500,
        "z": 7
      }
    ]
  }
}
```

## 项目结构

```bash
ExportEngine/
├── CMakeLists.txt               # 顶层 CMake 构建脚本
├── ExportEngine.sln              # VS2022 解决方案（Windows）
├── test_scene.json               # 示例场景文件
├── ExportEngine/                 # 核心渲染库
│   ├── CMakeLists.txt
│   ├── SceneData.h               # 数据模型（Canvas、Rect、Circle、Text、Color 等）
│   ├── JsonSceneParser.h/cpp     # JSON 场景解析器
│   ├── SceneRenderer.h/cpp       # 渲染引擎（SDF 填充 + AGG 描边 + 字体渲染）
│   ├── Color/                    # 颜色转换（IColorConverter 接口 + ColorConverter/ColorTransform）
│   ├── Sampling/                 # 像素缩放（IResampler 接口 + NearestResampler）
│   ├── Sink/                     # 输出接收器（IRenderSink 接口 + TiffSink/MemorySink/CallbackSink/NullSink）
│   ├── TiffWriter.h/cpp          # TIFF 文件写入（流式：strip 生产者-消费者）
│   ├── ProgressManager.h/cpp     # 任务进度管理
│   └── Version.h/cpp             # 版本信息
├── App/EETool/                   # 命令行 JSON→TIFF 转换工具
│   ├── CMakeLists.txt
│   └── main.cpp                  # 入口
├── App/EEVerify/                 # 渲染验证工具
├── Test/                         # 测试用例（JSON 场景文件）
│   ├── rects/                    # 矩形测试
│   ├── circles/                  # 椭圆/圆测试
│   ├── lines/                    # 直线测试
│   ├── freelines/                # 自由线测试
│   ├── beziers/                  # 贝塞尔曲线测试
│   └── texts/                    # 文字渲染测试
├── 3rdParty/                     # 预置第三方库（Windows）
├── ICC Profile/                  # ICC 色彩配置文件
└── bin/                          # 构建输出
```

## 版本

当前版本：**0.1.0**

获取版本号：

```cpp
#include "Version.h"
const char* ver = Version::full();  // "0.1.0 (2026-06-04 10:34:00)"
```
