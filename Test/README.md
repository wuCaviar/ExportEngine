# ExportEngine 测试用例

本目录包含 ExportEngine 的各种图元测试用例 JSON 文件。

## 平台支持

| 平台    | 字体引擎                        | 状态             |
| ------- | ------------------------------- | ---------------- |
| macOS   | FreeType + CoreText             | 实心填充、抗锯齿 |
| Linux   | FreeType + fontconfig（计划中） | —                |
| Windows | GDI（win32_tt）                 | 实心填充、抗锯齿 |

macOS 依赖：FreeType（字体轮廓）、CoreText（字体名称 → 文件路径查找）。

## 构建

### macOS

```bash
# 依赖
brew install freetype libtiff little-cms2

# 构建
cmake -B build -S .
make -C build -j$(sysctl -n hw.ncpu)

# 输出
# 可执行文件: bin/Debug/EETool
# 动态库:     bin/Debug/libExportEngine.dylib
```

### Windows

```bash
# 使用 CMake + Visual Studio
cmake -B build -S .
cmake --build build --config Release
```

## 目录结构

```bash
Test/
├── rects/          # 矩形图元测试
│   ├── rect_basic.json       # 基础矩形（填充）
│   ├── rect_rounded.json     # 圆角矩形
│   ├── rect_linestyle.json   # 线条样式（实线、虚线、点线等）
│   ├── rect_gradient.json    # 渐变填充（线性、径向、锥形）
│   ├── rect_grid.json        # 网格填充（透明背景、圆角、CMYK、mm 单位）
│   └── rect_texture.json     # 图片纹理填充（平铺、原图大小 DPI 换算、偏移、透明 PNG）
│                              #   注意：EEVerify 预期 FAIL —— rect 6 故意同时设置 fillColor +
│                              #   textureFill（渲染路径的既有警告场景）；渲染路径才是验收路径
│
├── circles/        # 圆形/椭圆图元测试
│   ├── circle_basic.json     # 基础圆形和椭圆
│   └── circle_gradient.json  # 渐变填充圆形
│
├── lines/          # 直线图元测试
│   └── line_basic.json       # 各种线条样式和宽度
│
├── freelines/      # 自由线图元测试
│   └── freeline_basic.json   # 自由线条
│
├── beziers/        # 贝塞尔曲线图元测试
│   └── bezier_basic.json     # 二次和三次贝塞尔曲线
│
└── texts/          # 文字图元测试
    ├── text_basic.json       # 基础文字（字体、颜色、对齐）
    └── text_chinese.json     # 中文文字测试
```

## 使用方法

### 运行单个测试

```bash
# macOS
./bin/Debug/EETool Test/texts/text_basic.json output/text_basic.tif

# Windows
./bin/Release/EETool.exe Test/rects/rect_basic.json

# 输出文件
./output/*.tif
```

### 运行所有测试

```bash
# macOS / Linux
for f in Test/**/*.json; do
  echo "=== $f ==="
  bin/Debug/EETool "$f" "output/$(basename ${f%.json}).tif"
done

# Windows (CMD)
for %f in (Test\**\*.json) do bin\Release\EETool.exe %f
```

## 图元属性说明

### 通用属性

| 属性          | 类型   | 说明                |
| ------------- | ------ | ------------------- |
| x, y          | double | 位置坐标            |
| width, height | double | 尺寸                |
| z             | double | Z-order（层叠顺序） |
| fillColor     | Color  | 填充颜色            |
| strokeColor   | Color  | 描边颜色            |
| strokeWidth   | double | 描边宽度            |
| lineStyle     | string | 线条样式            |

### 颜色格式

```json
{
  "c": 0, // CMYK: 0-100
  "m": 0,
  "y": 0,
  "k": 0,
  "a": 1.0 // 透明度: 0-1
}
```

### 线条样式

- `solid` - 实线
- `dashed` - 虚线
- `dotted` - 点线
- `dashDot` - 点划线
- `doubleDotDash` - 双点划线
- `none` - 无

### 渐变类型

- `linear` - 线性渐变
- `radial` - 径向渐变
- `conic` - 锥形渐变

## 字体说明

macOS 通过 CoreText 将字体家族名称（`fontFamily`）解析为实际字体文件路径。仅系统已安装的字体可用。

### 中文文字渲染

测试用例 `texts/text_chinese.json` 默认使用 Windows 字体（SimSun、SimHei、KaiTi、Microsoft YaHei）。这些字体在 macOS 上默认为缺失状态，会导致文字不渲染。如需在 macOS 上测试中文，请将 `fontFamily` 替换为 macOS 系统字体：

| Windows 字体                | macOS 替代         |
| --------------------------- | ------------------ |
| SimSun（宋体）              | STSong / Songti SC |
| SimHei（黑体）              | STHeiti / Heiti SC |
| KaiTi（楷体）               | STKaiti / Kaiti SC |
| Microsoft YaHei（微软雅黑） | PingFang SC        |

也可以通过安装对应的 `.ttf` / `.otf` 字体文件，使这些 Windows 字体在 macOS 上可用。

### 字体查找优先级

1. 按 `fontFamily` + `bold`/`italic` 特征精确匹配（如 "Arial" + Bold）
2. 若无精确匹配，回退至常规体（Regular）
3. 若仍查找失败，`createFont()` 返回 false，文字不渲染

## 添加新测试

1. 在对应图元目录下创建新的 JSON 文件
2. 遵循现有的文件命名规范
3. 确保 JSON 格式正确
4. 运行测试验证渲染结果
