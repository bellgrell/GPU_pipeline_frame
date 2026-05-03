# GPU 渲染管线多线程仿真 (GPU Rendering Pipeline Multi-threaded Simulation)
基于 C++ 多线程实现的完整 GPU 图形管线仿真器，严格复现经形式化验证的 Promela 模型逻辑，支持并行处理、原子深度测试、纹理采样与控制台渲染输出。
(A complete GPU graphics pipeline simulator based on C++ multi-threading, strictly reproducing the formally verified Promela model logic, supporting parallel processing, atomic depth test, texture sampling, and console rendering output.)

## 项目简介 (Project Overview)
本项目使用标准 C++ 线程、原子操作、信号量与线程安全通道，实现了一条可实际运行的固定功能 GPU 渲染流水线。
(This project uses standard C++ threads, atomic operations, semaphores, and thread-safe channels to implement a runnable fixed-function GPU rendering pipeline.)

仿真包含完整图形渲染流程：顶点处理 → 图元组装 → 光栅化 → 纹理映射 → 像素着色 → 深度测试 → 帧缓冲输出。
(The simulation covers the full graphics rendering flow: Vertex Processing → Primitive Assembly → Rasterization → Texture Mapping → Pixel Shading → Depth Test → Frame Buffer Output.)

## 核心功能 (Core Features)
- 完整 10 级 GPU 渲染管线并行仿真 (Full 10-stage GPU rendering pipeline parallel simulation)
- 2× 顶点着色器 / 像素着色器 / 纹理单元 / 渲染输出单元 (2× Vertex Shader / Pixel Shader / Texture Unit / Render Output Unit)
- 线程安全 Channel 通信（完全复现 Promela 通道语义）(Thread-safe Channel communication (fully reproduces Promela channel semantics))
- 原子 Z 深度测试，避免并发像素丢失 (Atomic Z-depth test to prevent concurrent pixel loss)
- 控制台 ASCII 字符渲染画面 (Console ASCII character rendering output)
- 全局时钟与管线性能统计 (Global clock and pipeline performance statistics)
- 无死锁、无竞态条件的多线程同步 (Deadlock-free & race-condition-free multi-threaded synchronization)

## 管线结构 (Pipeline Structure)
1. CPU 主线程（帧控制）(CPU Main Thread (Frame Control))
2. 顶点获取 VertexFetcher ×2 (Vertex Fetcher ×2)
3. 顶点处理 VertexProcessor ×2 (Vertex Processor ×2)
4. 图元组装 PrimitiveAssembler (Primitive Assembler)
5. 光栅化 Rasterizer (Rasterizer)
6. 纹理单元 TextureUnit ×2 (Texture Unit ×2)
7. 像素处理 PixelProcessor ×2 (Pixel Processor ×2)
8. 渲染输出 ROP ×2 (Render Output (ROP) ×2)
9. 帧缓冲 FrameBuffer (Frame Buffer)
10. 全局时钟 ClockGenerator (Clock Generator)

## 程序模型 (Program Model)
![Program Model](https://github.com/user-attachments/assets/f483a7db-e2a9-420f-aadc-38679e0876a7)

## 技术亮点 (Technical Highlights)
- 无第三方依赖，纯 C++ 标准库 (No third-party dependencies, pure C++ standard library)
- 多线程并行与信号量同步 (Multi-thread parallelism and semaphore synchronization)
- 原子操作实现无锁深度测试 (Lock-free depth test via atomic operations)
- 线程安全阻塞队列 Channel (Thread-safe blocking queue Channel)
- 与 Promela 形式化模型行为一致 (Consistent behavior with the Promela formal model)

## 适用场景 (Application Scenarios)
- 计算机图形学教学 (Computer graphics education)
- GPU 架构原理学习 (GPU architecture principle learning)
- 多线程并发验证 (Multi-threaded concurrency verification)
- 形式化模型代码迁移实验 (Formal model-to-code migration experiments)

## 相关学术工作 (Related Academic Work)
本项目基于经过 SPIN 模型检验的 GPU 管线形式化模型实现。
(This project is implemented based on the SPIN-verified formal model of the GPU pipeline.)

> Staroletov S. Formal Modeling and Verification of Various GPU Pipeline Architectures in SPIN. SPIN 2026.
