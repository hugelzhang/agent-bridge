# AgentBridge 项目踩坑记录

> 创建日期: 2026-06-28
> 最后更新: 2026-06-29
> 状态: 正在开发

## 已知坑位

_2026-06-29: 项目骨架搭建完毕，编译验证通过_

---

## 记录格式

每条坑包含:
- **日期**: 发现时间
- **现象**: 什么表现
- **根因**: 追溯到的原因
- **修复**: 怎么解决的
- **避免**: 如果重新来，怎么做能避开

---

## 编译/烧录坑

| 日期 | 现象 | 根因 | 修复 | 避免 |
|------|------|------|------|------|
| 2026-06-29 | `SyntaxError: invalid character '→' (U+2192)` 生成 PPT 脚本失败 | 中文引号 `""`（U+201C/U+201D）和 Python 字符串 `"`（U+0022）冲突；Windows 终端编码不是 UTF-8 | 用 Python 脚本把中文引号替换为 `「」`，把 `→` 替换为 `->` | 写中文内容的 Python 脚本，字符串分隔符用单引号 `'`，不要用双引号 `"` |
| 2026-06-29 | `agent_bridge.c:46: error: unknown type name 'va_list'` | 使用 `vsnprintf` 但没有 `#include <stdarg.h>` | 加 `#include <stdarg.h>` | 用 `vsnprintf`/`va_list` 系列函数必须包含 stdarg.h |
| 2026-06-29 | `agent_bridge.c:408: unused variable 'response' [-Werror]` | `handle_tool_call` 中声明了 `response` 变量但没使用；ESP-IDF 编译开了 `-Werror` | 删除未使用变量 | 提交前用 `-Wall -Werror` 自检 |
| 2026-06-29 | `build_tool_schema` / `json_get_method` defined but not used | 这两个静态函数预留用但当前没调用；ESP-IDF `-Werror` 把 warning 升级为 error | `__attribute__((unused))` 标记 | 预留给外部调用的工具函数别标 `static`，或者用属性标记 |
| 2026-06-29 | `fatal error: mcp_server.h: No such file or directory` transport_mcp.cpp 编译失败 | `transport_mcp.cpp` 放在 `components/agent_bridge/` 里，但它依赖 `main/` 中的 `mcp_server.h`；ESP-IDF 不允许组件间循环依赖 | 把 `transport_mcp.cpp/h` 移到 `main/` 目录，作为主应用的一部分编译 | 依赖应用层头文件的适配代码，放在应用层（main），不要放进通用组件 |
| 2026-06-29 | `passing 'const PropertyList' as 'this' argument discards qualifiers` | McpServer 的 `PropertyList::begin()/end()` 不是 const 成员函数，lambda 里 `const PropertyList& args` 无法遍历 | lambda 参数改为 `PropertyList args`（按值传递，去掉 const） | C++ 中设计容器类时要提供 const 迭代器 |
| 2026-06-29 | `undefined reference to 'transport_mcp_dispatch'` 链接失败 | `transport_mcp.cpp` 声明了 `extern "C"` 函数 `transport_mcp_dispatch`，但 `agent_bridge.c` 中实际函数名是 `agent_bridge_dispatch_tool` | 改调 `agent_bridge_dispatch_tool`，这是 agent_bridge.h 公开 API | 先在 .h 里找有没有现成 API 再声明 extern |
| 2026-06-29 | `designator order for field 'agent_device::ops' does not match declaration order` (×3) | C++ 中 designated initializer 必须按 struct 声明顺序；`.hw_ctx` 在 `.ops` 之前，但 struct 声明顺序是 `.ops` 在前；`.ops` 内部 `.set_position` 在 `.get_state` 之前，但声明顺序相反 | 调整初始化顺序：`.ops` 在 `.hw_ctx` 前，`.get_state` 在 `.set_position` 前 | 用 C++ designated initializer 时严格按照 struct 声明顺序排列字段 |
| 2026-06-29 | `invalid conversion from 'int' to 'agent_capability_t'` | C++ 中 `AB_CAP_ON_OFF \| AB_CAP_LEVEL` 结果是 `int`，不能隐式转换回 enum | `(agent_capability_t)(AB_CAP_ON_OFF \| AB_CAP_LEVEL)` 强制转换 | C++ 中位或枚举值需要显式转换回枚举类型 |
| 2026-06-29 | `lv_imgfont_create` implicit declaration — xiaozhi-fonts 组件编译失败 | managed_components 自动下载的版本和原始 sdkconfig/LVGL 版本不匹配 | 从原 phase7 工程复制完整 managed_components 目录 | 移植 ESP-IDF 项目时，连同 managed_components 一起复制，不要依赖 `idf.py reconfigure` 重新下载 |
| 2026-06-29 | `ml307_ssl.cc` 编译失败（4G 模组 SSL 组件） | 78__esp-ml307 组件中 SSL 相关代码和当前 ESP-IDF 5.5.4 工具链不兼容 | 未修复——4.3C 板用 WiFi 不需要 ML307。后续可通过 sdkconfig 关闭 4G 组件 | ESP-IDF 项目移植时检查 sdkconfig 的 board type，关闭不需要的通信模组 |
| 2026-06-29 | ninja minidump crash (0xC0000005) + "paging file is too small" | ESP-IDF 全量构建（2414 编译单元）内存不足，Windows pagefile 太小 | 未解决——改用交叉编译器 `xtensa-esp-elf-gcc` 单独验证 AgentBridge 代码 | Windows 上构建大型 ESP-IDF 项目（2400+ 编译单元）至少需要 16GB RAM + 足够 pagefile |

---

## 项目结构坑

| 日期 | 现象 | 根因 | 修复 | 避免 |
|------|------|------|------|------|
| 2026-06-29 | ESP-IDF `idf.py` 在 Git Bash 中无法使用 | ESP-IDF v5.5 不支持 MSys/Mingw 环境；`idf.py.exe` wrapper 需要 CMD 或 PowerShell | 改用 `cmd.exe /c` 或 PowerShell 运行构建；编译验证用交叉编译器直接编译 | Windows 上 ESP-IDF 开发用 ESP-IDF Command Prompt 或 PowerShell，不要用 Git Bash |
| 2026-06-29 | 全量复制 phase7 时 managed_components 太大（数百 MB）不能一起拷贝 | managed_components 包含字体文件、二进制 blob 等大文件 | 先不拷贝，让 `idf.py reconfigure` 下载；编译失败后改为从原工程复制 | 把 managed_components 加到 .gitignore；项目移植时从原工程同步 |

---

## 协议/通信坑

| 日期 | 现象 | 根因 | 修复 | 避免 |
|------|------|------|------|------|
| - | - | - | - | - |

---

## Windows 环境坑

| 日期 | 现象 | 根因 | 修复 | 避免 |
|------|------|------|------|------|
| 2026-06-29 | Python 脚本中文字符乱码/语法错误 | Windows GBK 编码与 UTF-8 源文件不兼容 | 在脚本开头加 `# -*- coding: utf-8 -*-`；全局替换中文引号和特殊 Unicode 字符为 ASCII 等价物 | 跨平台 Python 脚本避免使用 `""`（中文引号）、`→` 等非 ASCII 字符当语法元素 |
| 2026-06-29 | cmd.exe + PowerShell 构建日志中文乱码 | Windows 终端编码问题 | 不影响编译结果，日志只看英文部分 | 构建脚本中尽量用英文输出 |
