# EEVerify — ExportEngine JSON Scene Validator

在渲染之前检查 JSON 场景描述文件，发现格式错误、类型问题、数值越界和逻辑约束违反，给出定位和修复建议。

## 快速开始

```bash
# 校验单个文件
EEVerify test_scene.json

# 查看帮助
EEVerify --help
```

## 输出示例

```bash
==============================================================
  EEVerify - ExportEngine JSON Validator
==============================================================

File: test_scene.json
Lines: 156

ERROR   [canvas.rects[2].fillColor.c]
        CMYK.c = -5 is outside the normal range [0, 100]
        → Clamp c to 0-100 for physically meaningful color.

WARNING [canvas.circles[0].gradient.stops]
        Gradient stops are not sorted by offset in ascending order
        → Reorder stops so offset values increase from 0.0 to 1.0 for correct gradient rendering.

HINT    [canvas.fontFamily]
        Unknown field "fontFamily"
        → Did you mean "fontFamily"?

--------------------------------------------------------------
  Summary: 1 error(s), 1 warning(s), 1 hint(s)
  Overall: FAILED (errors found)
==============================================================
```

## 校验范围

### JSON 结构

| 检查项          | 级别  | 说明                              |
| --------------- | ----- | --------------------------------- |
| JSON 语法       | Error | `json::parse()` 语法错误          |
| 根节点类型      | Error | 必须是 Object                     |
| 必须含 `canvas` | Error | 缺少顶层 canvas 键                |
| 未知顶层键      | Hint  | 提示可能的拼写错误 (如 `canvass`) |

### Canvas

| 检查项             | 级别    | 说明                                         |
| ------------------ | ------- | -------------------------------------------- |
| `width` / `height` | Error   | 必须为正整数                                    |
| `unit`             | Error   | 仅支持 `"px"` 和 `"mm"`，未指定时默认 `"px"` |
| `dpi`              | Warning | 建议 72-2400                                 |
| `background`       | Hint    | 未指定时提示将透明                           |

### 颜色

| 检查项    | 级别    | 说明                    |
| --------- | ------- | ----------------------- |
| `space`   | Error   | 仅支持 `cmyk` 和 `rgba` |
| CMYK 通道 | Warning | 范围 0-100              |
| RGBA 通道 | Warning | 范围 0-255              |
| Alpha     | Error   | 范围 0.0-1.0            |

### 渐变

| 检查项        | 级别    | 说明                           |
| ------------- | ------- | ------------------------------ |
| `type`        | Error   | 仅支持 linear / radial / conic |
| `stops` 数量  | Error   | 至少 2 个色标                  |
| `offset` 范围 | Error   | 0.0-1.0                        |
| offset 排序   | Warning | 必须升序                       |
| radial `r`    | Warning | 必须为正                       |

### 图元

| 图元        | 检查项                            | 级别    | 说明                                          |
| ----------- | --------------------------------- | ------- | --------------------------------------------- |
| Rect        | `width` / `height`                | Warning | 必须 > 0                                      |
| Rect        | `cornerRadius`                    | Hint    | 超过尺寸一半时提示                            |
| Circle      | `radiusX`                         | Error   | 必须 > 0                                      |
| Circle      | `radiusY`                         | Warning | 必须 > 0                                      |
| FreeLine    | `points`                          | Error   | 至少 2 个点                                   |
| FreeLine    | 点坐标                            | Error   | 每个点必须有 x, y                             |
| Bezier      | `controlPoints`                   | Error   | 至少 2 个点                                   |
| Bezier      | 控制点数量                        | Hint    | 非标准数量组合                                |
| Line        | 起止相同                          | Warning | 直线退化为点                                  |
| Text        | `content`                         | Error   | 必须为非空字符串                              |
| Text        | `fontSize`                        | Error   | 必须 > 0                                      |
| Text        | `fontFamily`                      | Warning | 空字符串时提示                                |
| Image       | `filePath`                        | Error   | 必须为非空字符串                              |
| Image       | `filePath` 扩展名                 | Warning | 仅支持 .tiff / .tif                           |
| Image       | `width` / `height`                | Warning | 不能为负，0 表示自动                          |
| 所有图元    | `unit`                            | Error   | 仅支持 `"px"` 或 `"mm"`，未指定时继承画布单位 |
| 所有图元    | `strokeWidth`                     | Warning | 不能为负                                      |
| 所有图元    | `lineStyle`                       | Error   | 6 种合法值之一                                |
| Rect/Circle | `fillColor` + `gradient` 同时存在 | Error   | 两者互斥，只能使用一种                        |
| 所有图元    | `rotation`                        | Hint    | 所有图元均不支持旋转，视为未知字段           |
| 所有图元    | `gradient` 嵌套在颜色内           | Error   | 旧格式，gradient 已移至图元顶层               |
| 所有图元    | 未知字段                          | Hint    | 检测拼写错误，给出最近匹配                    |

## 单位系统

EEVerify 会校验画布和图元的 `unit` 字段，确保值为 `"px"` 或 `"mm"`。

**继承规则：**

- 画布 `unit` 定义全局默认单位，缺省为 `"px"`
- 每个图元可单独声明 `unit` 覆盖
- 图元未声明时继承画布单位

**受影响的字段：** 位置坐标（`x`, `y`, `cx`, `cy`, `x1`, `y1`, `x2`, `y2`，点数组中的坐标）、几何尺寸（`width`, `height`, `radiusX`, `radiusY`）、描边宽度（`strokeWidth`）、圆角半径（`cornerRadius`）。

**不受影响的字段：** `fontSize`（始终为 pt）、`z`、颜色值、`lineStyle` 等。

## 错误级别

| 级别        | 含义     | 不修复的后果             |
| ----------- | -------- | ------------------------ |
| **Error**   | 必须修复 | 渲染失败或结果错误       |
| **Warning** | 应该修复 | 可能导致非预期行为       |
| **Hint**    | 建议优化 | 可正常渲染，但有改进空间 |

## 返回值

| 值  | 含义                             |
| --- | -------------------------------- |
| `0` | 通过 (无 Error)，可安全渲染      |
| `1` | 失败 (存在 Error 或文件不可访问) |

## 拼写纠错

对于未知的 JSON 字段，EEVerify 会尝试匹配最近的已知字段名：

```bash
HINT    [canvas.rects[0].strokeColour]
        Unknown field "strokeColour"
        → Did you mean "strokeColor"?
```

以 `_` 或 `$` 开头的键会被视为注释/元数据，不触发未知字段提示。

## 不依赖项

EEVerify 仅依赖 `nlohmann/json.hpp` (项目已包含在 `3rdParty/`)，不需要 ExportEngine.dll 或其他运行时库。

## 与 EETool 的关系

| 工具         | 功能                   |
| ------------ | ---------------------- |
| **EEVerify** | 静态校验 JSON 文件格式 |
| **EETool**   | 实际渲染 JSON → TIFF   |

建议工作流：先 `EEVerify` 校验，通过后再用 `EETool` 渲染。
