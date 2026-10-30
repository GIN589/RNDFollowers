# RNDFollowers

给追随者加上 Realistic Needs and Diseases (RND, USSEP 版) 的饥饿、口渴、疲劳和疾病。

## 文件
| 文件 | 放哪里 |
| --- | --- |
| `RNDFollowers.dll`（自己编译） | `Data/SKSE/Plugins/` |
| `RNDFollowers.ini` | `Data/SKSE/Plugins/` |
| `RNDFollowers_Forms.ini`（xEdit 脚本生成） | `Data/SKSE/Plugins/` |
| `RNDFollowers.esp`（xEdit 脚本生成） | `Data/`，加载顺序在 RND 之后 |

## 它做什么
- 速率、阶段阈值、食物点数、疾病几率都来自 RND 自己的全局变量，改 RND 的设置会同步影响追随者。
- 惩罚来自 `RNDFollowers.esp` 里复制自 RND 的 Ability（已去掉脚本和“仅玩家”条件）。
- 追随者装备（吃/喝）带 RND 食物效果的物品时减少对应需求。生食、变质食物、河水可能让追随者生病。
- 玩家睡觉（含 Sleep in Bed SKSE、Spooning of Skyrim）时附近的追随者一起休息，追随者自己躺在床上也会被识别。
- 疾病按 RND 的规则逐级加重，睡眠后有机会减轻，Cure Disease 药水或神殿可治愈。

## 编译
1. 安装 Visual Studio 2022（C++ 桌面开发）、CMake 和 vcpkg。
2. 按 CommonLibSSE-NG 官方项目模板配置 vcpkg 的 registry。
3. 在本目录运行：
   `cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake`
   `cmake --build build --config Release`

## 卸载
把 `RNDFollowers.ini` 里的 `Uninstall=true`，进游戏并让追随者在身边，存档后再删除 DLL 和 ESP。

## 已知未验证项
- 代码未在真实游戏中测试，第一次编译可能需要修改少量 CommonLibSSE-NG API 名称。
- 食物点数对应的全局变量由 RND 的 MGEF 属性推断得出。
- 追随者吃东西的方式如果不触发“装备物品”事件，就不会被识别。
