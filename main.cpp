#include <iostream>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <chrono>

// 依赖头文件（与原Promela工程匹配）
#include "gpu_verticles.h"
#include "gpu_triangles.h"
#include "gpu_trigo.h"
#include "gpu_adjacency.h"

// ==================== 原子像素结构（恢复标准深度测试） ====================
struct AtomicPixel {
    std::atomic<int> depth;
    std::atomic<unsigned char> color;

    AtomicPixel() : depth(32767), color(0) {}

    // 标准深度测试：近处像素覆盖远处像素，解决过量写入问题
    bool atomic_depth_test_and_write(int new_depth, unsigned char new_color) {
        int expected_depth = depth.load(std::memory_order_relaxed);

        while (true) {
            if (new_depth >= expected_depth) {
                return false;
            }

            if (depth.compare_exchange_weak(expected_depth, new_depth,
                                            std::memory_order_acq_rel,
                                            std::memory_order_relaxed)) {
                color.store(new_color, std::memory_order_release);
                return true;
            }
        }
    }
};

// ==================== GPU管线配置（与原Promela完全一致） ====================
#define VERTEX_SHADERS 2
#define PIXEL_SHADERS 2
#define TMUS 2
#define ROPS 2

#define FB
#define DEBUG

#define TOTAL_FRAMES 1
#define FRAME_TIME 10000
#define CLOCK_STAT 3000

#define MX 200
#define MY 60
#define MX2 (MX/2)
#define MY2 (MY/2)

#define MAX_FRAGMENTS (MX*MY)
#define MAX_PIXELS (MX*MY)

// ==================== 线程安全通道实现 ====================
template<typename T>
class Channel {
private:
    std::queue<T> queue;
    mutable std::mutex mutex;
    std::condition_variable not_empty;
    std::condition_variable not_full;
    size_t capacity;
    bool closed = false;

public:
    Channel(size_t cap = 20000) : capacity(cap) {}

    bool send(T value) {
        std::unique_lock<std::mutex> lock(mutex);
        not_full.wait(lock, [this]() {
            return queue.size() < capacity || closed;
        });

        if (closed) return false;

        queue.push(value);
        not_empty.notify_one();
        return true;
    }

    bool receive(T& value) {
        std::unique_lock<std::mutex> lock(mutex);
        not_empty.wait(lock, [this]() {
            return !queue.empty() || closed;
        });

        if (closed && queue.empty()) return false;

        value = queue.front();
        queue.pop();
        not_full.notify_one();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex);
        closed = true;
        not_empty.notify_all();
        not_full.notify_all();
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mutex);
        return closed;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return queue.size();
    }
};

// ==================== 信号量实现 ====================
class Semaphore {
private:
    std::mutex mutex;
    std::condition_variable cond;
    int count;

public:
    Semaphore(int initial = 0) : count(initial) {}

    void wait() {
        std::unique_lock<std::mutex> lock(mutex);
        cond.wait(lock, [this]() { return count > 0; });
        count--;
    }

    void signal() {
        std::lock_guard<std::mutex> lock(mutex);
        count++;
        cond.notify_one();
    }

    int get_count()  {
        std::lock_guard<std::mutex> lock(mutex);
        return count;
    }
};

// ==================== 全局渲染数据结构 ====================
int vertex_u[MAX_VERTICES];
int vertex_v[MAX_VERTICES];
std::atomic<int> vertex_ready[MAX_VERTICES];

std::atomic<int> triangle_ready[MAX_TRIANGLES];
std::atomic<int> tri_ready_count[MAX_TRIANGLES];

// 片段数据结构
struct FragmentData {
    int id;
    int x, y, z;
    int u, v;
    unsigned char tex_color;
    unsigned char color_r;
};

FragmentData fragments[MAX_FRAGMENTS];
std::atomic<int> current_fragment_id(0);

// 原子帧缓冲
AtomicPixel atomic_frame_buffer[(MX + 1) * (MY + 1)];

// 全局统计计数器（原子操作保证线程安全）
std::atomic<int> triangles_assembled(0);
std::atomic<int> fragments_generated(0);
std::atomic<int> total_pixels_written(0);
std::atomic<int> pixels_lost(0);

std::atomic<int> current_frame(0);
std::atomic<int> total_frames_completed(0);

// 顶点原始坐标
float vertex_orig_x[MAX_VERTICES];
float vertex_orig_y[MAX_VERTICES];
float vertex_orig_z[MAX_VERTICES];

// 纹理配置
#define TEX_SIZE 16
unsigned char texture[TEX_SIZE * TEX_SIZE];

// 渲染参数
std::atomic<int> angle(99);
std::atomic<int> global_clock(0);
std::atomic<bool> clock_3000_shown(false);

// 管线进度计数器
std::atomic<int> textured_fragments(0);
std::atomic<int> shaded_fragments(0);
std::atomic<int> rop_processed(0);

// ==================== 全局管线通道 ====================
Channel<int> cpu_to_vf_channel(20000);
Channel<int> vf_to_vp_channel(20000);
Channel<int> vp_to_pa_channel(20000);
Channel<int> pa_to_rast_channel(20000);
Channel<int> rast_to_tmu_channel(20000);
Channel<int> tmu_to_pp_channel(20000);
Channel<int> pp_to_rop_channel(20000);
Channel<int> rop_to_fb_channel(20000);

// ==================== 全局同步信号量 ====================
Semaphore frame_start_sem(0);
Semaphore frame_complete_sem(0);
Semaphore vertex_processed_sem(0);
Semaphore triangle_ready_sem(0);
Semaphore fragment_ready_sem(0);
Semaphore texture_done_sem(0);
Semaphore shading_done_sem(0);
Semaphore rop_done_sem(0);

// 控制台输出互斥锁
std::mutex cout_mutex;
std::atomic<bool> simulation_done(false);

// ==================== macOS 线程调度优化 ====================
void macos_yield() {
    std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::microseconds(2));
}

// ==================== 初始化函数 ====================
// 初始化棋盘格纹理
void init_texture() {
    for (int y = 0; y < TEX_SIZE; y++) {
        for (int x = 0; x < TEX_SIZE; x++) {
            if (((x/4 + y/4) % 2) == 0) {
                texture[y * TEX_SIZE + x] = 200;
            } else {
                texture[y * TEX_SIZE + x] = 100;
            }
        }
    }

    for (int x = 0; x < MAX_VERTICES; x++) {
        vertex_u[x] = 15;
        vertex_v[x] = 0;
    }
}

// 清空帧缓冲
void clear_frame_buffer() {
    for (int i = 0; i < (MX + 1) * (MY + 1); i++) {
        atomic_frame_buffer[i].depth.store(32767, std::memory_order_relaxed);
        atomic_frame_buffer[i].color.store(0, std::memory_order_relaxed);
    }
}

// 渲染帧缓冲到控制台
void display_frame_buffer(int frame, int pixels) {
#ifdef FB
    std::lock_guard<std::mutex> lock(cout_mutex);
    std::cout << "--- Frame " << frame << " (pixels: " << pixels << ") ---" << std::endl;

    const char shades[8] = {' ', '.', ':', 'o', 'O', '8', '@', '#'};

    for (int yy = 0; yy < MY; yy++) {
        std::cout << "|";
        for (int xx = 0; xx < MX; xx++) {
            unsigned char pixel_val = atomic_frame_buffer[yy * MX + xx].color.load(std::memory_order_relaxed);
            if (pixel_val == ' ') {
                std::cout << ' ';
            } else if (pixel_val < 255) {
                int char_index = (pixel_val * 7) / 255;
                if (char_index >= 0 && char_index < 8) {
                    std::cout << shades[char_index];
                } else {
                    std::cout << ' ';
                }
            } else {
                std::cout << static_cast<char>(pixel_val);
            }
        }
        std::cout << std::endl;
    }
#endif
}

// 更新时钟并打印统计信息
void update_clock(int ticks) {
    int new_clock = global_clock.fetch_add(ticks, std::memory_order_relaxed) + ticks;

    if (new_clock >= CLOCK_STAT && !clock_3000_shown.exchange(true)) {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\n=== ClockGenerator: Tick " << new_clock << " ===" << std::endl;
        std::cout << "Statistics at tick " << new_clock << ":" << std::endl;
        std::cout << "  Vertices processed: " << (triangles_assembled.load() * 3) << std::endl;
        std::cout << "  Triangles created: " << triangles_assembled.load() << std::endl;
        std::cout << "  Fragments generated: " << fragments_generated.load() << std::endl;
        std::cout << "  Pixels written: " << total_pixels_written.load() << std::endl;
        std::cout << "  Texture progress: " << textured_fragments.load() << "/" << fragments_generated.load() << std::endl;
    }
}

// ==================== CPU 主线程 ====================
void cpu_thread() {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "CPU: started" << std::endl;
    }

    while (!simulation_done) {
        frame_start_sem.wait();

        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << "\nCPU: Preparing frame " << current_frame.load() << std::endl;
            std::cout << "CPU: Start of frame " << current_frame.load() << std::endl;
        }

        for (int i = 0; i < MAX_VERTICES; i++) {
            if (!cpu_to_vf_channel.send(i)) break;

            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "CPU[frame " << current_frame.load() << "]: Vertex " << i << " sent" << std::endl;
            }

            std::this_thread::sleep_for(std::chrono::microseconds(1));
            macos_yield();
        }

        frame_complete_sem.wait();
    }
}

// ==================== 顶点获取线程 ====================
void vertex_fetcher_thread(int id) {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "VertexFetcher " << id << ": started" << std::endl;
    }

    while (!simulation_done) {
        int vid;
        if (cpu_to_vf_channel.receive(vid)) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "VF" << id << "[frame " << current_frame.load() << "]: Fetching vertex " << vid << std::endl;
            }

            if (vf_to_vp_channel.send(vid)) {
                {
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cout << "VF" << id << ": Vertex " << vid << " sent to processing" << std::endl;
                }
            }

            update_clock(5);
            macos_yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
}

// ==================== 顶点处理线程 ====================
void vertex_processor_thread(int id) {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "VertexProcessor " << id << ": started" << std::endl;
    }

    while (!simulation_done) {
        int vid;
        if (vf_to_vp_channel.receive(vid)) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "VP" << id << "[frame " << current_frame.load() << "]: Processing vertex " << vid << std::endl;
            }

            float orig_x = vertex_orig_x[vid];
            float orig_y = vertex_orig_y[vid];
            float orig_z = vertex_orig_z[vid];

            float rotated_x, rotated_y, rotated_z;
            int angle_val = angle.load() % 360;
            int sin_a = sin_table[angle_val];
            int cos_a = cos_table[angle_val];

            rotated_x = (orig_x * cos_a - orig_z * sin_a) / 1000.0f;
            rotated_z = (orig_x * sin_a + orig_z * cos_a) / 2000.0f;
            rotated_y = orig_y;

            float screen_x, screen_y;
            float focal = 2000.0f;

            if (rotated_z + focal != 0) {
                screen_x = (rotated_x * focal) / (rotated_z + focal);
                screen_y = (rotated_y * focal) / (rotated_z + focal);
            } else {
                screen_x = rotated_x;
                screen_y = rotated_y;
            }

            // 恢复原版模型缩放比例，不放大
            screen_x = MX2 - screen_x / 100.0f;
            screen_y = MY2 - screen_y / 100.0f;

            vertex_x[vid] = screen_x;
            vertex_y[vid] = screen_y;
            vertex_z[vid] = rotated_z;
            vertex_ready[vid] = 2;

            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "VP" << id << ": Vertex " << vid << " processed ("
                          << static_cast<int>(orig_x) << " " << static_cast<int>(orig_y) << " " << static_cast<int>(orig_z)
                          << ") -> (x=" << static_cast<int>(screen_x) << ", y=" << static_cast<int>(screen_y) << ")" << std::endl;
                std::cout << "VP" << id << "[frame " << current_frame.load() << "]: Vertex " << vid << " processing completed" << std::endl;
            }

            update_clock(30);

            vp_to_pa_channel.send(vid);
            vertex_processed_sem.signal();
            macos_yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
}

// ==================== 图元组装线程 ====================
void primitive_assembler_thread() {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "PrimitiveAssembler: started" << std::endl;
    }

    while (!simulation_done) {
        int vid;
        if (vp_to_pa_channel.receive(vid)) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "PA[frame " << current_frame.load() << "]: Vertex " << vid << " ready, checking associated triangles" << std::endl;
            }

            if (vertex_ready[vid] == 2) {
                int start = vertex_adj_start[vid];
                int count = vertex_adj_count[vid];

                for (int t = 0; t < count; t++) {
                    int tid = adj_tri_ids[start + t];

                    int remaining = tri_ready_count[tid].fetch_sub(1, std::memory_order_acq_rel) - 1;

                    {
                        std::lock_guard<std::mutex> lock(cout_mutex);
                        std::cout << "PA[frame " << current_frame.load() << "]: vid = " << vid << " , tid = " << tid
                                  << ", tri_ready_count = " << remaining << std::endl;
                    }

                    if (remaining == 0) {
                        triangle_ready[tid] = 1;
                        triangles_assembled.fetch_add(1, std::memory_order_relaxed);

                        {
                            std::lock_guard<std::mutex> lock(cout_mutex);
                            std::cout << "PA[frame " << current_frame.load() << "]: Triangle " << tid << " created" << std::endl;
                        }

                        pa_to_rast_channel.send(tid);
                        triangle_ready_sem.signal();
                    }
                }
            }
            macos_yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
}

// ==================== 光栅化线程（恢复单面渲染） ====================
void rasterizer_thread() {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "Rasterizer: started" << std::endl;
    }

    while (!simulation_done) {
        int tid;
        if (pa_to_rast_channel.receive(tid)) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "Rast[frame " << current_frame.load() << "]: Rasterizing triangle " << tid << std::endl;
            }

            int v0 = triangle_v0[tid];
            int v1 = triangle_v1[tid];
            int v2 = triangle_v2[tid];

            int x0 = static_cast<int>(vertex_x[v0]);
            int y0 = static_cast<int>(vertex_y[v0]);
            int z0 = static_cast<int>(vertex_z[v0]);
            int x1 = static_cast<int>(vertex_x[v1]);
            int y1 = static_cast<int>(vertex_y[v1]);
            int z1 = static_cast<int>(vertex_z[v1]);
            int x2 = static_cast<int>(vertex_x[v2]);
            int y2 = static_cast<int>(vertex_y[v2]);
            int z2 = static_cast<int>(vertex_z[v2]);

            int u0 = vertex_u[v0], v0_tex = vertex_v[v0];
            int u1 = vertex_u[v1], v1_tex = vertex_v[v1];
            int u2 = vertex_u[v2], v2_tex = vertex_v[v2];

            int min_x = std::min({x0, x1, x2});
            int max_x = std::max({x0, x1, x2});
            int min_y = std::min({y0, y1, y2});
            int max_y = std::max({y0, y1, y2});

            if (min_x < 0) min_x = 0;
            if (max_x >= MX) max_x = MX - 1;
            if (min_y < 0) min_y = 0;
            if (max_y >= MY) max_y = MY - 1;

            if (min_x > max_x || min_y > max_y) {
                continue;
            }

            int area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
            if (area == 0) {
                continue;
            }

            int fragments_start = current_fragment_id.load();
            int fragment_count = 0;

            for (int y = min_y; y <= max_y; y++) {
                for (int x = min_x; x <= max_x; x++) {
                    int w0 = (x1 - x2) * (y - y2) - (y1 - y2) * (x - x2);
                    int w1 = (x2 - x0) * (y - y0) - (y2 - y0) * (x - x0);
                    int w2 = (x0 - x1) * (y - y1) - (y0 - y1) * (x - x1);

                    // 恢复原版：仅渲染三角形正面，解决像素过量问题
                    if (w0 > 0 && w1 > 0 && w2 > 0) {
                        int alpha = (w0 * 1000) / area;
                        int beta = (w1 * 1000) / area;
                        int gamma = 1000 - alpha - beta;

                        int frag_z = (z0 * alpha + z1 * beta + z2 * gamma) / 1000;
                        int frag_u = (u0 * alpha + u1 * beta + u2 * gamma) / 1000;
                        int frag_v = (v0_tex * alpha + v1_tex * beta + v2_tex * gamma) / 1000;

                        frag_u = frag_u & 0xFF;
                        frag_v = frag_v & 0xFF;

                        int fid = current_fragment_id.fetch_add(1, std::memory_order_relaxed);
                        if (fid >= MAX_FRAGMENTS) {
                            break;
                        }

                        fragments[fid].id = fid;
                        fragments[fid].x = x;
                        fragments[fid].y = y;
                        fragments[fid].z = frag_z;
                        fragments[fid].u = frag_u;
                        fragments[fid].v = frag_v;

                        fragments_generated.fetch_add(1, std::memory_order_relaxed);
                        fragment_count++;

                        rast_to_tmu_channel.send(fid);
                        fragment_ready_sem.signal();
                    }
                }
            }

            int fragments_end = current_fragment_id.load() - 1;

            if (fragments_end >= fragments_start) {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "Rast: Created fragments " << fragments_start << ".." << fragments_end
                          << " for triangle " << tid << std::endl;
            }

            update_clock(20);
            macos_yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
}

// ==================== 纹理单元线程 ====================
void texture_unit_thread(int id) {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "TextureUnit " << id << ": started" << std::endl;
    }

    while (!simulation_done) {
        int fid;
        if (rast_to_tmu_channel.receive(fid)) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "TMU" << id << "[frame " << current_frame.load() << "]: Texturing fragment " << fid << std::endl;
            }

            int u = fragments[fid].u;
            int v = fragments[fid].v;

            int tex_x = (u * TEX_SIZE) >> 8;
            int tex_y = (v * TEX_SIZE) >> 8;

            if (tex_x < 0) tex_x = 0;
            if (tex_x >= TEX_SIZE) tex_x = TEX_SIZE - 1;
            if (tex_y < 0) tex_y = 0;
            if (tex_y >= TEX_SIZE) tex_y = TEX_SIZE - 1;

            unsigned char texel = texture[tex_y * TEX_SIZE + tex_x];
            fragments[fid].tex_color = texel;

            textured_fragments.fetch_add(1, std::memory_order_relaxed);

            if (textured_fragments.load() % 3 == 0 || textured_fragments.load() == fragments_generated.load()) {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "TMU0..1[frame " << current_frame.load() << "]:  " << textured_fragments.load() << "/" << fragments_generated.load() << "..." << std::endl;
            }

            tmu_to_pp_channel.send(fid);
            texture_done_sem.signal();

            update_clock(10);
            macos_yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
}

// ==================== 像素处理线程 ====================
void pixel_processor_thread(int id) {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "PixelProcessor " << id << ": started" << std::endl;
    }

    while (!simulation_done) {
        int fid;
        if (tmu_to_pp_channel.receive(fid)) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "PP" << id << "[frame " << current_frame.load() << "]: Shading fragment " << fid << std::endl;
            }

            unsigned char texel = fragments[fid].tex_color;
            int x = fragments[fid].x;
            int y = fragments[fid].y;

            // 原版光照计算，保留真实渲染效果
            int position_factor = (x * 3 + y * 7) % 100;
            int final_color = texel;

            if (position_factor < 30) {
                final_color = final_color * 9 / 10;
            } else if (position_factor > 70) {
                final_color = std::min(255, final_color * 11 / 10);
            }

            if (final_color < 0) final_color = 0;
            if (final_color > 255) final_color = 255;

            fragments[fid].color_r = static_cast<unsigned char>(final_color);

            shaded_fragments.fetch_add(1, std::memory_order_relaxed);

            pp_to_rop_channel.send(fid);
            shading_done_sem.signal();

            update_clock(15);
            macos_yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
}

// ==================== ROP 渲染输出线程 ====================
void rop_thread(int id) {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "ROP " << id << ": started" << std::endl;
    }

    while (!simulation_done) {
        int fid;
        if (pp_to_rop_channel.receive(fid)) {
            {
                std::lock_guard<std::mutex> lock(cout_mutex);
                std::cout << "ROP" << id << "[frame " << current_frame.load() << "]: Processing fragment " << fid << std::endl;
            }

            int x = fragments[fid].x;
            int y = fragments[fid].y;
            int z = fragments[fid].z;
            unsigned char r = fragments[fid].color_r;

            if (x >= 0 && x < MX && y >= 0 && y < MY) {
                int buffer_idx = y * MX + x;

                bool z_test_ok = atomic_frame_buffer[buffer_idx].atomic_depth_test_and_write(z, r);

                if (z_test_ok) {
                    total_pixels_written.fetch_add(1, std::memory_order_relaxed);

                    {
                        std::lock_guard<std::mutex> lock(cout_mutex);
                        std::cout << "ROP" << id << ": z-test for x=" << x << " y=" << y << " OK (z=" << z << ")" << std::endl;
                        std::cout << "FB[frame " << current_frame.load() << "]: Received pixel " << total_pixels_written.load()
                                  << " (for now we have " << total_pixels_written.load() << ")" << std::endl;
                    }
                } else {
                    std::lock_guard<std::mutex> lock(cout_mutex);
                    std::cout << "ROP" << id << ": z-test for x=" << x << " y=" << y
                              << " NOT OK (z=" << z << ", zbuf=" << atomic_frame_buffer[buffer_idx].depth.load(std::memory_order_relaxed) << ")" << std::endl;
                    pixels_lost.fetch_add(1, std::memory_order_relaxed);
                }
            }

            rop_processed.fetch_add(1, std::memory_order_relaxed);

            rop_to_fb_channel.send(fid);
            rop_done_sem.signal();

            update_clock(5);
            macos_yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    }
}

// ==================== 帧缓冲管理线程 ====================
void frame_buffer_thread() {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "FrameBuffer: started" << std::endl;
    }

    int fragments_received = 0;

    while (!simulation_done) {
        int fid;
        if (rop_to_fb_channel.receive(fid)) {
            fragments_received++;

            // 三条件同步：所有三角形组装完成+所有片段处理完成+所有片段接收完成
            bool all_triangles_assembled = (triangles_assembled.load() == MAX_TRIANGLES);
            bool all_fragments_processed = (rop_processed.load() == fragments_generated.load());
            bool all_fragments_received = (fragments_received == fragments_generated.load());

            if (all_triangles_assembled && all_fragments_processed && all_fragments_received) {
                frame_complete_sem.signal();
                fragments_received = 0;
            }
        }
        macos_yield();
    }
}

// ==================== 时钟生成线程 ====================
void clock_generator_thread() {
    {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "ClockGenerator: started" << std::endl;
    }

    while (!simulation_done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        global_clock.fetch_add(10, std::memory_order_relaxed);
        macos_yield();
    }
}

// ==================== 单帧渲染执行函数 ====================
void run_frame(int frame) {
    std::cout << "\n=== INITIALIZATION OF GRAPHICS PIPELINE ===" << std::endl;
    std::cout << "GPU Configuration: " << VERTEX_SHADERS << " VS, " << PIXEL_SHADERS
              << " PS, " << TMUS << " TMU, " << ROPS << " ROP" << std::endl;
    std::cout << "Frames to process: " << TOTAL_FRAMES << ", time per frame: " << FRAME_TIME << " ticks" << std::endl;

    current_frame = frame;
    global_clock = 0;
    clock_3000_shown = false;

    // 重置状态
    for (int i = 0; i < MAX_VERTICES; i++) {
        vertex_ready[i] = 0;
    }
    for (int i = 0; i < MAX_TRIANGLES; i++) {
        tri_ready_count[i] = 3;
        triangle_ready[i] = 0;
    }

    current_fragment_id = 0;
    triangles_assembled = 0;
    fragments_generated = 0;
    total_pixels_written = 0;
    textured_fragments = 0;
    shaded_fragments = 0;
    rop_processed = 0;

    clear_frame_buffer();

    std::cout << "\n=== SIMULATION STARTED ===" << std::endl;
    frame_start_sem.signal();
    frame_complete_sem.wait();

    // 渲染并输出结果
    display_frame_buffer(frame, total_pixels_written.load());

    total_frames_completed++;

    // 打印帧统计
    std::cout << "\n=== FRAME COMPLETED ===" << std::endl;
    std::cout << "Frame " << frame << " completed: " << total_pixels_written.load() << " pixels written" << std::endl;
    std::cout << "Vertices processed: " << MAX_VERTICES << std::endl;
    std::cout << "Triangles assembled: " << triangles_assembled.load() << std::endl;
    std::cout << "Fragments generated: " << fragments_generated.load() << std::endl;
    std::cout << "Total clock ticks: " << global_clock.load() << std::endl;
}

// ==================== 主函数 ====================
int main() {
    std::cout << "\n=== GPU PIPELINE SIMULATION (STABLE 2000+ PIXELS) ===" << std::endl;

    // 初始化依赖模块
    init_vertices();
    init_triangles();
    init_trig_tables();
    init_texture();
    init_adjacency();

    // 备份原始顶点坐标
    for (int i = 0; i < MAX_VERTICES; i++) {
        vertex_orig_x[i] = vertex_x[i];
        vertex_orig_y[i] = vertex_y[i];
        vertex_orig_z[i] = vertex_z[i];
    }

    angle = 99;

    // 创建所有管线线程
    std::vector<std::thread> threads;
    threads.emplace_back(clock_generator_thread);
    threads.emplace_back(cpu_thread);
    threads.emplace_back(vertex_fetcher_thread, 0);
    threads.emplace_back(vertex_fetcher_thread, 1);
    threads.emplace_back(vertex_processor_thread, 0);
    threads.emplace_back(vertex_processor_thread, 1);
    threads.emplace_back(primitive_assembler_thread);
    threads.emplace_back(rasterizer_thread);
    threads.emplace_back(texture_unit_thread, 0);
    threads.emplace_back(texture_unit_thread, 1);
    threads.emplace_back(pixel_processor_thread, 0);
    threads.emplace_back(pixel_processor_thread, 1);
    threads.emplace_back(rop_thread, 0);
    threads.emplace_back(rop_thread, 1);
    threads.emplace_back(frame_buffer_thread);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 执行渲染
    for (int frame = 1; frame <= TOTAL_FRAMES; frame++) {
        run_frame(frame);
        angle += 2;
    }

    // 停止模拟并回收线程
    simulation_done = true;
    cpu_to_vf_channel.close();
    vf_to_vp_channel.close();
    vp_to_pa_channel.close();
    pa_to_rast_channel.close();
    rast_to_tmu_channel.close();
    tmu_to_pp_channel.close();
    pp_to_rop_channel.close();
    rop_to_fb_channel.close();

    for (auto& thread : threads) {
        thread.join();
    }

    // 最终统计输出
    std::cout << "\n=== SIMULATION COMPLETED ===" << std::endl;
    std::cout << "Total frames processed: " << total_frames_completed << std::endl;
    std::cout << "Total pixels written: " << total_pixels_written.load() << std::endl;
    std::cout << "Total occluded pixels: " << (fragments_generated.load() - total_pixels_written.load()) << std::endl;

    return 0;
}
