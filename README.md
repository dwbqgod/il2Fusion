TextExtractTool
================

基于 LSPosed + native hook（Dobby 预留位）的 Unity 文本截获/替换工具骨架。

架构概览
--------
- LSPosed 模块入口：`com.tools.module.MainHook`（由 `META-INF/xposed_init` 指定），在目标进程的 `Application.attach` 后加载 `libnative_hook.so`，调用 JNI。
- 配置桥：`HookConfigStore` 通过系统剪贴板同步用户填的 RVA 列表（避免跨进程读写 SharedPreferences 的权限问题），`MainHook` 启动时下发到 native。
- UI 插件：Compose 界面 `HookConfigScreen` 支持多行 RVA 输入（十六进制/十进制），提示一次只勾选一个应用。
- Native 框架：`native_hook.cpp` 负责等待 `libil2cpp.so`、解析 `il2cpp_string_new`、按 RVA 安装 hook 并强制写入目标文本（示例 `"Hook Test"`）。当前链接的是 Dobby 源码（已做轻量兼容修改）。
- LSPosed 元数据：`META-INF/xposed/module.prop`、`META-INF/xposed_init`、`META-INF/native_init` 打包入 assets。

使用方法
--------
1) 在插件界面输入目标 RVA  
   - 打开 app，填写目标游戏 `libil2cpp.so` 中 `set_text` 的 RVA（十六进制可用 `0x` 前缀，多行支持多个目标），点击“保存到本地”。  
2) LSPosed 勾选目标应用  
   - 在 LSPosed 里仅勾选一个目标应用启用本模块（多选会互相覆盖，日志有提示）。  
3) 启动目标应用验证  
   - 启动游戏后，模块会在 `Application.attach` 时加载 `libnative_hook.so`，等待 `libil2cpp.so` 后安装 hook，并把第二个参数替换为 `"Hook Test"`；logcat 标签 `[TextExtractTool]`/`DobbyStub` 可观测。  
4) 构建  
   - `./gradlew :app:assembleDebug`（已启用 CMake，so 名称 `libnative_hook.so`）。  

关于 Dobby
----------
- 现在直接使用预编译的静态库 `app/src/main/cpp/libs/<abi>/libdobby.a` + 头文件 `dobby.h`，未做源码改动；CMake 以 IMPORTED STATIC 方式链接。
- 仓库附带 arm64/armeabi-v7a/x86/x86_64 的 `.a`，但 Gradle 默认 abiFilter 只包含 `arm64-v8a`。如需其它 ABI，补齐对应库并调整 `ndk.abiFilters`。

目录指引
--------
- `app/src/main/java/com/tools/module/`：LSPosed 入口、JNI 桥。  
- `app/src/main/java/com/tools/textextracttool/`：插件 UI 与配置存取。  
- `app/src/main/cpp/`：native hook 逻辑、Dobby 占位。  
- `META-INF/`：LSPosed 模块声明与入口文件。  

注意事项
--------
- 只在单一目标进程中启用模块，避免多应用同时 hook。  
- RVA 需对应目标版本的 `libil2cpp.so`，否则 hook 失效或崩溃。  
- 若更换替换文本或增加多语言，请在 `native_hook.cpp` 中调整目标字符串生成逻辑。  
