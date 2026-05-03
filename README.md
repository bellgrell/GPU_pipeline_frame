# GPU 渲染管线多线程仿真
基于 C++ 多线程实现的完整 GPU 图形管线仿真器，严格复现经形式化验证的 Promela 模型逻辑，支持并行处理、原子深度测试、纹理采样与控制台渲染输出。

## 项目简介
本项目使用标准 C++ 线程、原子操作、信号量与线程安全通道，实现了一条可实际运行的固定功能 GPU 渲染流水线。
仿真包含完整图形渲染流程：顶点处理 → 图元组装 → 光栅化 → 纹理映射 → 像素着色 → 深度测试 → 帧缓冲输出。

## 核心功能
- 完整 10 级 GPU 渲染管线并行仿真
- 2× 顶点着色器 / 像素着色器 / 纹理单元 / 渲染输出单元
- 线程安全 Channel 通信（完全复现 Promela 通道语义）
- 原子 Z 深度测试，避免并发像素丢失
- 控制台 ASCII 字符渲染画面
- 全局时钟与管线性能统计
- 无死锁、无竞态条件的多线程同步

## 管线结构
1. CPU 主线程（帧控制）
2. 顶点获取 VertexFetcher ×2
3. 顶点处理 VertexProcessor ×2
4. 图元组装 PrimitiveAssembler
5. 光栅化 Rasterizer
6. 纹理单元 TextureUnit ×2
7. 像素处理 PixelProcessor ×2
8. 渲染输出 ROP ×2
9. 帧缓冲 FrameBuffer
10. 全局时钟 ClockGenerator

## 编译与运行
```bash
g++ -std=c++11 main.cpp -o gpu_sim -pthread
./gpu_sim
```

运行后自动开始单帧渲染，并在控制台输出最终 3D 画面。

## 典型输出
<img width="6850" height="3336" alt="mermaid-diagram-2026-05-02-224125" src="https://github.com/user-attachments/assets/f483a7db-e2a9-420f-aadc-38679e0876a7" />


## 技术亮点
- 无第三方依赖，纯 C++ 标准库
- 多线程并行与信号量同步
- 原子操作实现无锁深度测试
- 线程安全阻塞队列 Channel
- 与 Promela 形式化模型行为一致

## 适用场景
- 计算机图形学教学
- GPU 架构原理学习
- 多线程并发验证
- 形式化模型代码迁移实验

## 相关学术工作
本项目基于经过 SPIN 模型检验的 GPU 管线形式化模型实现。
> Staroletov S. Formal Modeling and Verification of Various GPU Pipeline Architectures in SPIN. SPIN 2026.
